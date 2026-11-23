// usdcc_node's options (docs/RENDER_NODE.md §4), every command's, in one parser.

#include "node.h"

namespace usdcc::node {

void Options::add(QCommandLineParser& parser) {
    parser.setApplicationDescription(QStringLiteral(
        "usdcc_node renders usdcc's stages on a machine with nobody at it (docs/RENDER_NODE.md).\n\n"
        "Commands:\n"
        "  render <stage>   render a stage's frames, then exit (0 done, 1 failed, 2 bad arguments, 130 cancelled)\n"
        "  renderers        list the renderers, and why one can't render here\n"
        "  serve            render for peers in render queue rooms, until stopped\n"
        "  queue            a render queue server: takes artists' jobs and hands them to nodes, until stopped"));
    parser.addHelpOption();
    // Not addVersionOption(): it prints only the version, and this says what the build has too.
    parser.addOption({QStringLiteral("version"), QStringLiteral("Print the version and the features this build has, then exit.")});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("render, renderers, serve or queue"));
    parser.addPositionalArgument(QStringLiteral("stage"), QStringLiteral("render: the USD file"), QStringLiteral("[stage]"));
    // Every command.
    parser.addOption({QStringLiteral("profile"), QStringLiteral("The profile: its settings, logs and key (default usdcc-node)."),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("no-plugins"), QStringLiteral("Don't load any plugins.")});
    parser.addOption({QStringLiteral("plugin-path"), QStringLiteral("Also look for plugins in the folders inside <folder>. Repeatable."),
                      QStringLiteral("folder")});
    parser.addOption({QStringLiteral("no-gpu"),
                      QStringLiteral("No OpenGL: a QCoreApplication, and Storm isn't offered.")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("render, renderers: JSON lines on stdout.")});
    // render.
    parser.addOption({QStringLiteral("settings"), QStringLiteral("render: the RenderSettings prim (default: the stage's)."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("frames"), QStringLiteral("render: 12, 1-24 or 1-24x2 (default: the settings' range)."),
                      QStringLiteral("range")});
    parser.addOption({QStringLiteral("renderer"), QStringLiteral("render: a renderer id (usdcc_node renderers)."),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("camera"), QStringLiteral("render: the camera prim."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("resolution"), QStringLiteral("render: 1920x1080."), QStringLiteral("WxH")});
    parser.addOption({QStringLiteral("output"),
                      QStringLiteral("render: the files' pattern, relative to the working folder (renders/<stage>.####.exr)."),
                      QStringLiteral("pattern")});
    parser.addOption({QStringLiteral("format"), QStringLiteral("render: exr or png."), QStringLiteral("format")});
    parser.addOption({QStringLiteral("tiles"), QStringLiteral("render: 2x2."), QStringLiteral("XxY")});
    parser.addOption({QStringLiteral("time-limit"), QStringLiteral("render: seconds per task; 0 until done."),
                      QStringLiteral("seconds")});
    parser.addOption({QStringLiteral("set"),
                      QStringLiteral("render: a renderer's setting, as on the RenderSettings prim (embree:ty:maxBounces=8). "
                                     "Repeatable."),
                      QStringLiteral("attribute=value")});
    // serve.
    parser.addOption({QStringLiteral("join"), QStringLiteral("serve: a room's invite link, joined at start and after a lost "
                                                             "connection. Repeatable."),
                      QStringLiteral("link")});
    parser.addOption({QStringLiteral("public"), QStringLiteral("serve: join every public render queue room found here.")});
    parser.addOption({QStringLiteral("trust"), QStringLiteral("serve: a fingerprint (or its first eight, abcd-efgh) to render "
                                                              "for. Repeatable."),
                      QStringLiteral("fingerprint")});
    parser.addOption({QStringLiteral("trust-file"), QStringLiteral("serve: a file of fingerprints, one a line; read again when it "
                                                                   "changes."),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("key-file"), QStringLiteral("serve: the node's key pair, made there if it isn't."),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("name"), QStringLiteral("serve: the name peers see (default: this machine's)."),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("renderers"), QStringLiteral("serve: only these renderers, by id, comma separated."),
                      QStringLiteral("ids")});
    parser.addOption({QStringLiteral("inbox"), QStringLiteral("serve: where the scenes peers send are kept."),
                      QStringLiteral("folder")});
    // queue.
    parser.addOption({QStringLiteral("host-room"), QStringLiteral("queue: host a render queue room of this name."),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("queue: the hosted room's port (default: any free one)."),
                      QStringLiteral("port")});
    parser.addOption({QStringLiteral("local"), QStringLiteral("queue: the hosted room on this machine only (127.0.0.1).")});
    parser.addOption({QStringLiteral("accept"), QStringLiteral("queue: an artist's fingerprint (or abcd-efgh) to take jobs from. "
                                                               "Repeatable."),
                      QStringLiteral("fingerprint")});
    parser.addOption({QStringLiteral("accept-file"), QStringLiteral("queue: a file of artists' fingerprints, one a line."),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("admin"), QStringLiteral("queue: a fingerprint that may cancel, retry and remove anyone's "
                                                              "jobs. Repeatable."),
                      QStringLiteral("fingerprint")});
    parser.addOption({QStringLiteral("state"), QStringLiteral("queue: where jobs, scenes and finished tiles are kept, to go on "
                                                              "after a restart."),
                      QStringLiteral("folder")});
    parser.addOption({QStringLiteral("output-root"), QStringLiteral("queue: where the frames are written (a shared path); "
                                                                    "without it the artists fetch them."),
                      QStringLiteral("folder")});
    parser.addOption({QStringLiteral("render"), QStringLiteral("queue: render on this machine too.")});
    parser.addOption({QStringLiteral("web"), QStringLiteral("queue: serve the queue's web page on this port."),
                      QStringLiteral("port")});
    parser.addOption({QStringLiteral("web-token-file"), QStringLiteral("queue: the web page's token for Cancel, Retry and "
                                                                       "Remove, made there if it isn't."),
                      QStringLiteral("file")});
}

}  // namespace usdcc::node
