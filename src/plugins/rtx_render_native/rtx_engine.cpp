#include "rtx_engine.h"

// delayimp.h requires the Win32 declarations; preserve this include order.
// clang-format off
#include <windows.h>
#include <delayimp.h>
// clang-format on
#include <ovrtx/ovrtx.h>
#include <ovrtx/ovrtx_config.h>
#include <ovrtx/ovrtx_logging.h>
#include <ovstage/ovstage.h>
#include <ovstage/ovstage_population.h>
#include <ovx/path_dictionary/path_dictionary.h>

#include <QDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "usdcc/core/log.h"

namespace {
HMODULE rtxLoader{}, stageLoader{};
FARPROC WINAPI resolveVendorLoader(unsigned event, PDelayLoadInfo info) {
    if (event != dliNotePreLoadLibrary) return nullptr;
    if (!strcmp(info->szDll, "ovrtx-dynamic.dll")) return reinterpret_cast<FARPROC>(rtxLoader);
    if (!strcmp(info->szDll, "ovstage-dynamic.dll")) return reinterpret_cast<FARPROC>(stageLoader);
    return nullptr;
}
HMODULE loadVendor(const QString& root, const char* name) {
    QString path = QDir::toNativeSeparators(root + "/bin/" + name);
    auto value = LoadLibraryExW(reinterpret_cast<const wchar_t*>(path.utf16()), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!value)
        throw std::runtime_error(
            QStringLiteral("Could not load %1 (Windows %2)").arg(path).arg(GetLastError()).toStdString());
    return value;
}
}  // namespace
extern "C" const PfnDliHook __pfnDliNotifyHook2 = resolveVendorLoader;

namespace usdcc::rtx {
struct RtxEngine::State {
    std::mutex mutex;
    Schedule schedule;
    QString rtxRoot, stageRoot, saveFile;
    std::shared_ptr<GpuFrame> saveFrame;
    std::shared_ptr<GpuBridge> bridge;
    EngineUpdate output;
};
namespace {
using State = RtxEngine::State;
using namespace std::chrono_literals;
ovx_string_t string(const QByteArray& value) { return {value.constData(), size_t(value.size())}; }
QString text(ovx_string_t value) {
    return value.ptr ? QString::fromUtf8(value.ptr, qsizetype(value.length)) : QString();
}
void check(ovrtx_api_status_t code, const char* operation) {
    if (code != OVRTX_API_SUCCESS)
        throw std::runtime_error(QStringLiteral("%1: %2").arg(operation, text(ovrtx_get_last_error())).toStdString());
}
void checkStage(ovstage_api_status_t code, const char* operation) {
    if (code != OVSTAGE_OK)
        throw std::runtime_error(QStringLiteral("%1: %2").arg(operation, text(ovstage_get_last_error())).toStdString());
}
void status(const std::shared_ptr<State>& state, QString value) {
    std::lock_guard lock(state->mutex);
    state->output.status = std::move(value);
}
bool accepts(const std::shared_ptr<State>& state, const Request& request) {
    std::lock_guard lock(state->mutex);
    return state->schedule.accepts(request.generation, request.revision);
}
void stageWait(ovstage_instance_t* stage, ovstage_enqueue_result_t operation) {
    checkStage(operation.status, "enqueue stage update");
    ovstage_op_wait_result_t wait{};
    for (;;) {
        auto code = ovstage_wait_op(stage, operation.op_index, 10000000, &wait);
        if (code == OVSTAGE_ERROR_TIMEOUT) continue;
        const bool failed = wait.error_op_id_count != 0;
        ovstage_release_op(stage, operation.op_index);
        checkStage(code, "wait stage update");
        if (failed) throw std::runtime_error("ovstage update failed");
        break;
    }
}
struct Runtime {
    QString root, stageRoot;
    std::vector<DLL_DIRECTORY_COOKIE> directories;
    bool initialized = false;
    std::mutex progressMutex;
    std::shared_ptr<State> progressState;
    uint64_t progressGeneration = 0, progressRevision = 0;
    // The SDK callback is process-global and can run on any thread. Route its
    // compilation messages only to the request currently executing SDK calls.
    struct ProgressScope {
        Runtime& runtime;
        ProgressScope(Runtime& runtime, const std::shared_ptr<State>& state, const Request& request)
            : runtime(runtime) {
            std::lock_guard lock(runtime.progressMutex);
            runtime.progressState = state;
            runtime.progressGeneration = request.generation;
            runtime.progressRevision = request.revision;
        }
        ~ProgressScope() {
            std::lock_guard lock(runtime.progressMutex);
            runtime.progressState.reset();
        }
    };
    void compilationProgress(const QString& message) {
        std::lock_guard lock(progressMutex);
        if (!progressState) return;
        std::lock_guard stateLock(progressState->mutex);
        if (progressState->schedule.accepts(progressGeneration, progressRevision))
            progressState->output.status = "Compiling RTX shaders — " + message +
                                           " (first render can take several minutes)";
    }
    void open(const State& state) {
        if (initialized) {
            if (root != state.rtxRoot || stageRoot != state.stageRoot)
                throw std::runtime_error("All RTX panels must use the same native runtime roots");
            return;
        }
        root = state.rtxRoot;
        stageRoot = state.stageRoot;
        for (const auto& package : {root, stageRoot}) {
            QStringList paths{package + "/bin", package + "/bin/plugins"};
            QDir plugins(package + "/bin/plugins");
            for (auto& name : plugins.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) paths << plugins.filePath(name);
            for (auto& path : paths)
                if (QDir(path).exists()) {
                    auto cookie =
                        AddDllDirectory(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()));
                    if (cookie) directories.push_back(cookie);
                }
        }
        if (!rtxLoader) rtxLoader = loadVendor(root, "ovrtx-dynamic.dll");
        if (!stageLoader) stageLoader = loadVendor(stageRoot, "ovstage-dynamic.dll");
        uint32_t major = 0, minor = 0, patch = 0;
        ovrtx_get_version(&major, &minor, &patch);
        if (major != 0 || minor != 6 || patch != 0) throw std::runtime_error("Native RTX requires ovrtx 0.6.0");
        auto gpu = QByteArray::number(state.bridge->deviceId());
        const ovrtx_config_entry_t entries[] = {
            ovrtx_config_entry_active_cuda_gpus(string(gpu)),
            ovrtx_config_entry_bool(OVRTX_CONFIG_ENABLE_CRASH_REPORTER, false),
        };
        ovrtx_config_t config{entries, std::size(entries)};
        check(ovrtx_initialize(&config).status, "initialize RTX");
        initialized = true;
        const QByteArray filter = "omni.rtx=info";
        const auto channelFilter = string(filter);
        check(ovrtx_set_log_callback(
            OVX_LOG_WARNING, &channelFilter,
            [](const ovx_log_message_t* message, void* context) {
                try {
                    const auto value = text(message->message);
                    const auto compilation = value.indexOf("compilation:");
                    const bool progress = value.contains("Waiting for") && compilation >= 0;
                    if (progress)
                        static_cast<Runtime*>(context)->compilationProgress(value.mid(compilation + 12).trimmed());
                    if (message->severity < OVX_LOG_WARNING && !progress) return;
                    usdcc::core::LogRecord record;
                    record.level = message->severity >= OVX_LOG_ERROR ? usdcc::core::LogLevel::Error
                                   : message->severity >= OVX_LOG_WARNING ? usdcc::core::LogLevel::Warning
                                                                         : usdcc::core::LogLevel::Info;
                    record.source = "plugin:org.usdcc.rtx-render";
                    record.category = "rtx.native.sdk";
                    record.message = text(message->channel).toStdString() + ": " + value.toStdString();
                    usdcc::core::LogHub::instance().post(std::move(record));
                } catch (...) { /* callbacks cannot throw into the vendor runtime */
                }
            },
            this).status, "set RTX log callback");
    }
    ~Runtime() {
        if (initialized) {
            ovrtx_set_log_callback(OVX_LOG_WARNING, nullptr, nullptr, nullptr);
            ovrtx_shutdown();
        }
        for (auto cookie : directories) RemoveDllDirectory(cookie);
    }
};
struct Session {
    ovrtx_renderer_t* renderer{};
    ovstage_instance_t* stage{};
    ovrtx_step_result_handle_t step{};
    ovrtx_render_var_output_handle_t color{};
    uint64_t generation = 0, revision = 0, ordinal = 0;
    int frame = 0;
    bool attached = false;
    std::shared_ptr<Snapshot> snapshot;
    ~Session() {
        if (step) ovrtx_destroy_results(renderer, step);
        if (attached) ovrtx_detach_ovstage(renderer);
        if (renderer) ovrtx_destroy_renderer(renderer);
        if (stage) ovstage_destroy_instance(stage);
    }
    void load(const std::shared_ptr<State>& state, const Request& request) {
        status(state, "Initializing RTX");
        ovrtx_config_t config{};
        check(ovrtx_create_renderer(&config, &renderer).status, "create renderer");
        ovstage_instance_desc_t desc{};
        desc.name = "usdcc.rtx.native";
        checkStage(ovstage_create_instance(&desc, &stage), "create stage");
        uint32_t major = 0, minor = 0, patch = 0;
        ovstage_get_version(stage, &major, &minor, &patch);
        if (major != 0 || minor != 3 || patch != 0) throw std::runtime_error("Native RTX requires ovstage 0.3.0");
        status(state, "Attaching the scene to RTX");
        check(ovrtx_attach_ovstage(renderer, stage).status, "attach ovstage");
        attached = true;
        if (!accepts(state, request)) return;
        status(state, "Loading the scene");
        snapshot = request.snapshot;
        const auto file = snapshot->file.toUtf8();
        auto op = ovstage_population_open_usd_from_file(stage, string(file), ++ordinal, OVSTAGE_POPULATION_DEFAULT_TIME,
                                                        OVSTAGE_POPULATION_DOMAIN_RENDERING);
        checkStage(op.status, "populate scene");
        ovstage_population_op_wait_result_t wait{};
        for (;;) {
            auto code = ovstage_population_wait_op(stage, op.op_index, 10000000, &wait);
            if (code == OVSTAGE_ERROR_TIMEOUT) continue;
            checkStage(code, "wait scene population");
            if (wait.error_op_id_count)
                throw std::runtime_error(
                    "Scene population failed: " +
                    text(ovstage_population_get_last_op_error(wait.error_op_ids[0])).toStdString());
            break;
        }
        commit();
        generation = request.generation;
        // The snapshot already contains these exact values. Subsequent updates
        // are the only writes needed; initial redundant writes can invalidate
        // authored schema interpretations on attributes such as clippingRange.
        if (request.transforms.empty() && request.camera.matrix == snapshot->view.matrix &&
            request.camera.horizontal == snapshot->view.horizontal &&
            request.camera.vertical == snapshot->view.vertical && request.size == snapshot->size)
            revision = request.revision;
    }
    void commit() {
        ovstage_write_floor_desc_t floor{};
        floor.ordinal = ordinal;
        floor.scope = OVSTAGE_SCOPE_ALL;
        stageWait(stage, ovstage_advance_write_floor(stage, &floor));
    }
    void write(const QString& path, const char* attribute, void* values, DLDataType dtype, std::vector<int64_t> shape,
               ovstage_attribute_semantic_t semantic = OVSTAGE_SEMANTIC_NONE) {
        auto* dictionary = ovstage_get_path_dictionary(stage);
        const auto encoded = path.toUtf8();
        const auto name = QByteArray(attribute);
        ovx_string_t stringPath = string(encoded);
        ovx_primpath_list_t paths{};
        ovstage_query_handle_t query{};
        auto created = path_dictionary_create_path_list_from_strings(dictionary, &stringPath, 1, &paths);
        if (created.status != OVX_API_SUCCESS) throw std::runtime_error("Could not create RTX prim path query");
        struct Cleanup {
            ovstage_instance_t* stage;
            path_dictionary_instance_t* dictionary;
            ovx_primpath_list_t paths;
            ovstage_query_handle_t& query;
            ~Cleanup() {
                if (query) {
                    try {
                        stageWait(stage, ovstage_release_query(stage, query));
                    } catch (...) {
                    }
                }
                path_dictionary_release_path_list_reference(dictionary, paths);
            }
        } cleanup{stage, dictionary, paths, query};
        checkStage(ovstage_query_from_path_list(stage, paths, &query), "query prim");
        std::vector<int64_t> strides(shape.size());
        int64_t stride = 1;
        for (size_t i = shape.size(); i-- > 0;) {
            strides[i] = stride;
            stride *= shape[i];
        }
        DLTensor tensor{};
        tensor.data = values;
        tensor.device = {kDLCPU, 0};
        tensor.ndim = int(shape.size());
        tensor.dtype = dtype;
        tensor.shape = shape.data();
        tensor.strides = strides.data();
        ovstage_write_data_t data{};
        data.tensors = &tensor;
        data.tensor_count = 1;
        data.semantic = semantic;
        ovx_string_or_token_t attr{};
        attr.string = string(name);
        stageWait(stage, ovstage_write_attribute(stage, query, attr, ordinal, data, OVSTAGE_PRIM_MODE_UPSERT));
    }
    void update(const Request& request) {
        if (revision == request.revision) return;
        ++ordinal;
        auto camera = request.camera;
        write(snapshot->camera, "omni:xform", camera.matrix.data(), {kDLFloat, 64, 1}, {1, 4, 4},
              OVSTAGE_SEMANTIC_MATRIX);
        write(snapshot->camera, "horizontalAperture", &camera.horizontal, {kDLFloat, 32, 1}, {1});
        write(snapshot->camera, "verticalAperture", &camera.vertical, {kDLFloat, 32, 1}, {1});
        float clipping[] = {camera.nearClip, camera.farClip};
        write(snapshot->camera, "clippingRange", clipping, {kDLFloat, 32, 1}, {1, 2});
        int size[] = {request.size.width(), request.size.height()};
        write(snapshot->product, "resolution", size, {kDLInt, 32, 1}, {1, 2});
        for (auto& [path, values] : request.transforms) {
            auto matrix = values;
            write(path, "omni:xform", matrix.data(), {kDLFloat, 64, 1}, {1, 4, 4}, OVSTAGE_SEMANTIC_MATRIX);
        }
        commit();
        revision = request.revision;
        frame = 0;
    }
    void render(const std::shared_ptr<State>& state, const Request& request) {
        if (request.mode == Mode::PathTracing) {
            uint32_t samples = sampleBudget(frame, request.budget);
            ++ordinal;
            write(snapshot->product, "omni:rtx:pt:samplesPerPixel", &samples, {kDLUInt, 32, 1}, {1});
            commit();
        }
        // Retain the last completed step for explicit Save Image readback.
        if (step) {
            ovrtx_destroy_results(renderer, step);
            step = 0;
            color = 0;
        }
        const auto product = snapshot->product.toUtf8();
        ovx_string_t path = string(product);
        ovrtx_render_product_set_t products{};
        products.render_products = &path;
        products.num_render_products = 1;
        status(state, QStringLiteral("Rendering — %1 %2")
                          .arg(request.mode == Mode::PathTracing ? "samples" : "frame")
                          .arg(request.mode == Mode::PathTracing ? sampleBudget(frame, request.budget) : frame + 1));
        auto operation = ovrtx_step_with_stage(renderer, products, 1.0 / 60, ordinal, &step);
        check(operation.status, "submit frame");
        ovrtx_op_wait_result_t wait{};
        for (;;) {
            auto code = ovrtx_wait_op(renderer, operation.op_index, {10000000}, &wait).status;
            if (code == OVRTX_API_TIMEOUT) continue;
            check(code, "wait frame");
            if (wait.num_error_ops) throw std::runtime_error("RTX rendering operation failed");
            if (!wait.lowest_pending_op_id || wait.lowest_pending_op_id > operation.op_index) break;
        }
        ovrtx_render_product_set_outputs_t outputs{};
        for (;;) {
            auto code = ovrtx_fetch_results(renderer, step, {0}, &outputs).status;
            if (code != OVRTX_API_TIMEOUT) {
                check(code, "fetch frame");
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        const auto varPath = snapshot->product + "/LdrColor";
        for (size_t p = 0; p < outputs.output_count; ++p)
            for (size_t f = 0; f < outputs.outputs[p].output_frame_count; ++f) {
                auto& output = outputs.outputs[p].output_frames[f];
                for (size_t v = 0; v < output.render_var_count; ++v)
                    if (text(output.output_render_vars[v].render_var_path) == varPath)
                        color = output.output_render_vars[v].output_handle;
            }
        if (!color) throw std::runtime_error("RTX returned no LdrColor image");
        if (!accepts(state, request)) return;
        ovrtx_map_output_description_t desc{};
        desc.device_type = OVRTX_MAP_DEVICE_TYPE_CUDA;
        desc.sync_stream = state->bridge->stream();
        ovrtx_render_var_output_t mapped{};
        for (;;) {
            auto code = ovrtx_map_render_var_output(renderer, color, &desc, {0}, &mapped).status;
            if (code != OVRTX_API_TIMEOUT) {
                check(code, "map CUDA frame");
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        struct Mapping {
            ovrtx_renderer_t* renderer;
            ovrtx_render_var_output_map_handle_t handle;
            uintptr_t stream;
            ~Mapping() { ovrtx_unmap_render_var_output(renderer, handle, {stream, 0}); }
        } mapping{renderer, mapped.map_handle, state->bridge->stream()};
        if (mapped.num_tensors != 1 || !mapped.tensors[0].dl) throw std::runtime_error("Unexpected RTX tensor count");
        std::shared_ptr<GpuFrame> image;
        while (accepts(state, request) && !(image = state->bridge->copy(*mapped.tensors[0].dl)))
            std::this_thread::sleep_for(2ms);
        if (image) {
            while (!image->copied()) std::this_thread::sleep_for(1ms);
            std::lock_guard lock(state->mutex);
            if (state->schedule.accepts(request.generation, request.revision)) {
                state->output.frame = std::move(image);
                state->output.generation = generation;
                state->output.revision = revision;
                ++frame;
                const bool done = request.mode == Mode::PathTracing
                                      ? sampleBudget(frame - 1, request.budget) >= request.budget
                                      : frame >= request.budget;
                state->output.done = done;
                if (done) state->output.status = "Done";
            }
        }
    }
};
class Service {
public:
    static Service& instance() {
        static auto* service = new Service;
        return *service;
    }
    void add(std::shared_ptr<State> state) {
        {
            std::lock_guard lock(mutex);
            if (stopping && !finished.load())
                throw std::runtime_error("RTX is still draining native work; retry after it finishes");
            if (finished.load() && worker.joinable()) worker.join();
            clients.push_back({std::move(state), {}});
            if (!worker.joinable()) {
                stopping = false;
                finished.store(false);
                worker = std::thread([this] { run(); });
            }
        }
        wake.notify_one();
    }
    void notify() { wake.notify_one(); }
    void removeUnstarted(const std::shared_ptr<State>& state) {
        std::lock_guard lock(mutex);
        std::erase_if(clients, [&](auto& client) { return client.state == state && !client.session; });
        wake.notify_one();
    }
    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (auto& client : clients) {
                std::lock_guard stateLock(client.state->mutex);
                client.state->schedule.closed = true;
                client.state->schedule.active = false;
                client.state->output.frame.reset();
                client.state->output.error = "RTX is stopping; waiting for pending native work to drain.";
            }
        }
        wake.notify_one();
    }
    bool shutdownFinished() {
        if (!finished.load(std::memory_order_acquire)) return false;
        if (worker.joinable()) worker.join();
        return true;
    }

private:
    struct Client {
        std::shared_ptr<State> state;
        std::unique_ptr<Session> session;
    };
    Service() = default;
    void run() {
        {
            Runtime runtime;
            for (;;) {
                std::vector<std::shared_ptr<State>> states;
                bool stop = false;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, 10ms);
                    stop = stopping;
                    for (auto& c : clients) states.push_back(c.state);
                }
                for (auto& state : states) {
                    // Only this worker changes sessions; add() may reallocate clients,
                    // so the session is moved out under the service mutex.
                    std::unique_ptr<Session> session;
                    {
                        std::lock_guard lock(mutex);
                        for (auto& c : clients)
                            if (c.state == state) {
                                session = std::move(c.session);
                                break;
                            }
                    }
                    Request request;
                    bool active, closed;
                    QString save;
                    std::shared_ptr<GpuFrame> saveFrame;
                    {
                        std::lock_guard lock(state->mutex);
                        closed = stop || state->schedule.closed;
                        active = state->schedule.active && state->schedule.visible;
                        request = state->schedule.desired;
                        save = std::exchange(state->saveFile, {});
                        saveFrame = std::exchange(state->saveFrame, {});
                    }
                    if (!closed) try {
                            if (!save.isEmpty()) {
                                try {
                                    if (!saveFrame) throw std::runtime_error("No displayed frame to save");
                                    saveFrame->save(save);
                                    std::lock_guard lock(state->mutex);
                                    state->output.saved = save;
                                } catch (const std::exception& error) {
                                    std::lock_guard lock(state->mutex);
                                    state->output.saveError = QString::fromUtf8(error.what());
                                }
                            }
                            if (active && request.snapshot) {
                                if (!runtime.initialized) status(state, "Initializing RTX");
                                runtime.open(*state);
                                Runtime::ProgressScope progress(runtime, state, request);
                                if (!session || session->generation != request.generation) {
                                    session.reset();  // fully replace: deleted prims cannot survive population
                                    session = std::make_unique<Session>();
                                    session->load(state, request);
                                }
                                if (session->generation == request.generation && accepts(state, request)) {
                                    try {
                                        session->update(request);
                                        std::lock_guard lock(state->mutex);
                                        for (auto& [path, matrix] : request.transforms) {
                                            auto found = state->schedule.desired.transforms.find(path);
                                            if (found != state->schedule.desired.transforms.end() &&
                                                found->second == matrix)
                                                state->schedule.desired.transforms.erase(found);
                                        }
                                    } catch (...) {
                                        std::lock_guard lock(state->mutex);
                                        state->output.reloadNeeded = true;
                                        state->schedule.active = false;
                                        throw;
                                    }
                                    const bool done =
                                        request.mode == Mode::PathTracing
                                            ? session->frame &&
                                                  sampleBudget(session->frame - 1, request.budget) >= request.budget
                                            : session->frame >= request.budget;
                                    if (!done) session->render(state, request);
                                }
                            }
                        } catch (const std::exception& error) {
                            std::lock_guard lock(state->mutex);
                            state->output.error = QString::fromUtf8(error.what());
                            state->schedule.active = false;
                        }
                    if (closed) session.reset();
                    {
                        std::lock_guard lock(mutex);
                        for (auto& c : clients)
                            if (c.state == state) {
                                c.session = std::move(session);
                                break;
                            }
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    std::erase_if(clients, [](auto& c) {
                        std::lock_guard stateLock(c.state->mutex);
                        return c.state->schedule.closed && !c.session;
                    });
                }
                if (stop) break;
            }
            // Runtime shuts down only after sessions/mappings have drained on this
            // worker. A vendor call which never returns cannot be force-cancelled.
        }
        finished.store(true, std::memory_order_release);
    }
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Client> clients;
    bool stopping = false;
    std::atomic<bool> finished{true};
    std::thread worker;
};
}  // namespace
RtxEngine::RtxEngine(QString rtxRoot, QString stageRoot, std::shared_ptr<GpuBridge> bridge)
    : m(std::make_shared<State>()) {
    m->rtxRoot = std::move(rtxRoot);
    m->stageRoot = std::move(stageRoot);
    m->bridge = std::move(bridge);
    m->output.status = "Waiting for native RTX worker";
    Service::instance().add(m);
}
RtxEngine::~RtxEngine() { close(); }
uint64_t RtxEngine::replace(std::shared_ptr<Snapshot> snapshot, int budget) {
    std::lock_guard lock(m->mutex);
    m->schedule.replace(std::move(snapshot), budget);
    m->output = {};
    m->output.status = "Waiting for native RTX worker";
    Service::instance().notify();
    return m->schedule.desired.generation;
}
void RtxEngine::update(Camera camera, QSize size, const std::map<QString, std::array<double, 16>>& transforms) {
    std::lock_guard lock(m->mutex);
    m->schedule.desired.camera = std::move(camera);
    m->schedule.desired.size = size;
    ++m->schedule.desired.revision;
    for (auto& [path, matrix] : transforms) m->schedule.desired.transforms[path] = matrix;
    m->output.done = false;
    m->output.frame.reset();
    Service::instance().notify();
}
void RtxEngine::setActive(bool active) {
    std::lock_guard lock(m->mutex);
    m->schedule.active = active;
    if (!active) m->output.frame.reset();
    Service::instance().notify();
}
void RtxEngine::setVisible(bool visible) {
    std::lock_guard lock(m->mutex);
    m->schedule.visible = visible;
    if (!visible) m->output.frame.reset();
    Service::instance().notify();
}
void RtxEngine::save(QString file, std::shared_ptr<GpuFrame> frame) {
    std::lock_guard lock(m->mutex);
    if (m->schedule.closed) {
        m->output.saveError = "RTX is stopping; the save request was not queued.";
        return;
    }
    m->saveFile = std::move(file);
    m->saveFrame = std::move(frame);
    Service::instance().notify();
}
uint64_t RtxEngine::generation() const {
    std::lock_guard lock(m->mutex);
    return m->schedule.desired.generation;
}
EngineUpdate RtxEngine::poll() {
    std::lock_guard lock(m->mutex);
    auto update = m->output;
    m->output.frame.reset();
    m->output.error.clear();
    m->output.saved.clear();
    m->output.saveError.clear();
    m->output.reloadNeeded = false;
    if (!m->schedule.accepts(update.generation, update.revision)) update.frame.reset();
    return update;
}
void RtxEngine::close() {
    {
        std::lock_guard lock(m->mutex);
        m->schedule.closed = true;
        m->schedule.active = false;
        m->schedule.desired.snapshot.reset();
        m->output.frame.reset();
        m->saveFrame.reset();
        m->saveFile.clear();
    }
    Service::instance().removeUnstarted(m);
}
void RtxEngine::shutdown() { Service::instance().shutdown(); }
bool RtxEngine::shutdownFinished() { return Service::instance().shutdownFinished(); }
bool RtxEngine::isClosed() const {
    std::lock_guard lock(m->mutex);
    return m->schedule.closed;
}
}  // namespace usdcc::rtx
