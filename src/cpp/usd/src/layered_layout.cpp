#include "usdcc/usd/layered_layout.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <numeric>
#include <utility>

namespace usdcc::usd {

namespace {

// A line as the layering sees it, going right: from tail's column to a column right of it.
struct Arc {
    int tail = 0;
    int head = 0;
    float tailY = 0.0f;  // where it leaves tail, down from tail's top
    float headY = 0.0f;  // where it reaches head
};

// ---- 1. Loops

// Which arcs to turn round so there's no loop: those a depth-first search finds going back up
// its own path. Starting from `first`, then what nothing feeds, keeps the authored direction
// wherever there's no loop.
std::vector<bool> loopBreakers(int count, const std::vector<Arc>& arcs, const std::vector<int>& first) {
    std::vector<std::vector<int>> outOf(count);
    for (std::size_t a = 0; a < arcs.size(); ++a) outOf[arcs[a].tail].push_back(static_cast<int>(a));
    std::vector<int> state(count, 0);  // 0 unseen, 1 on the path, 2 done
    std::vector<bool> turn(arcs.size(), false);
    const auto visit = [&](int root) {
        if (state[root] != 0) return;
        std::vector<std::pair<int, std::size_t>> path{{root, 0}};
        state[root] = 1;
        while (!path.empty()) {
            const int node = path.back().first;
            if (path.back().second < outOf[node].size()) {
                const int a = outOf[node][path.back().second++];
                const int head = arcs[a].head;
                if (state[head] == 1) {
                    turn[a] = true;
                } else if (state[head] == 0) {
                    state[head] = 1;
                    path.emplace_back(head, 0);
                }
            } else {
                state[node] = 2;
                path.pop_back();
            }
        }
    };
    std::vector<int> fed(count, 0);
    for (const Arc& arc : arcs) ++fed[arc.head];
    for (int n : first) visit(n);
    for (int n = 0; n < count; ++n) {
        if (fed[n] == 0) visit(n);
    }
    for (int n = 0; n < count; ++n) visit(n);
    return turn;
}

// ---- 2. Columns

// Network simplex: each arc at least one column long, the weighted length of all of them as
// short as can be. `arcs` mustn't loop and must connect all `count` nodes.
std::vector<int> networkSimplexRanks(int count, const std::vector<Arc>& arcs, const std::vector<float>& weight) {
    const int m = static_cast<int>(arcs.size());
    std::vector<int> rank(count, 0);
    std::vector<std::vector<int>> incident(count);
    std::vector<int> fed(count, 0);
    for (int a = 0; a < m; ++a) {
        incident[arcs[a].tail].push_back(a);
        incident[arcs[a].head].push_back(a);
        ++fed[arcs[a].head];
    }
    // A start that's allowed: the longest path from what nothing feeds.
    std::vector<int> ready;
    for (int n = 0; n < count; ++n) {
        if (fed[n] == 0) ready.push_back(n);
    }
    while (!ready.empty()) {
        const int n = ready.back();
        ready.pop_back();
        for (int a : incident[n]) {
            if (arcs[a].tail != n) continue;
            rank[arcs[a].head] = std::max(rank[arcs[a].head], rank[n] + 1);
            if (--fed[arcs[a].head] == 0) ready.push_back(arcs[a].head);
        }
    }
    if (count <= 1) return rank;
    const auto slack = [&](int a) { return rank[arcs[a].head] - rank[arcs[a].tail] - 1; };

    // A spanning tree of arcs with no slack, moving nodes to make one.
    std::vector<char> inTree(count, 0);
    std::vector<char> treeArc(m, 0);
    const auto grow = [&]() {
        std::fill(inTree.begin(), inTree.end(), 0);
        std::fill(treeArc.begin(), treeArc.end(), 0);
        inTree[0] = 1;
        int size = 1;
        std::vector<int> open{0};
        while (!open.empty()) {
            const int n = open.back();
            open.pop_back();
            for (int a : incident[n]) {
                const int other = arcs[a].tail == n ? arcs[a].head : arcs[a].tail;
                if (inTree[other] || slack(a) != 0) continue;
                inTree[other] = 1;
                treeArc[a] = 1;
                ++size;
                open.push_back(other);
            }
        }
        return size;
    };
    while (grow() < count) {
        int best = -1;
        int least = INT_MAX;
        for (int a = 0; a < m; ++a) {
            if (inTree[arcs[a].tail] != inTree[arcs[a].head] && slack(a) < least) {
                least = slack(a);
                best = a;
            }
        }
        if (best < 0) break;  // not connected: it's laid out as it is
        const int delta = inTree[arcs[best].head] ? -least : least;
        for (int n = 0; n < count; ++n) {
            if (inTree[n]) rank[n] += delta;
        }
    }

    // Which side of a tree arc a node is on, by the tree's postorder (Gansner's low and lim).
    std::vector<int> low(count), lim(count), parentArc(count, -1);
    std::vector<std::vector<int>> treeOf(count);
    const auto number = [&]() {
        for (auto& arcsOf : treeOf) arcsOf.clear();
        for (int a = 0; a < m; ++a) {
            if (!treeArc[a]) continue;
            treeOf[arcs[a].tail].push_back(a);
            treeOf[arcs[a].head].push_back(a);
        }
        int counter = 0;
        std::fill(parentArc.begin(), parentArc.end(), -1);
        std::vector<std::pair<int, std::size_t>> path{{0, 0}};
        low[0] = counter;
        while (!path.empty()) {
            const int n = path.back().first;
            if (path.back().second < treeOf[n].size()) {
                const int a = treeOf[n][path.back().second++];
                if (a == parentArc[n]) continue;
                const int child = arcs[a].tail == n ? arcs[a].head : arcs[a].tail;
                parentArc[child] = a;
                low[child] = counter;
                path.emplace_back(child, 0);
            } else {
                lim[n] = counter++;
                path.pop_back();
            }
        }
    };
    const auto tailSide = [&](int a) {
        const int child = parentArc[arcs[a].tail] == a ? arcs[a].tail : arcs[a].head;
        const bool childIsTail = child == arcs[a].tail;
        return [&, child, childIsTail](int n) {
            const bool below = low[child] <= lim[n] && lim[n] <= lim[child];
            return below == childIsTail;
        };
    };
    const auto reRank = [&]() {
        std::vector<char> seen(count, 0);
        std::vector<int> open{0};
        seen[0] = 1;
        while (!open.empty()) {
            const int n = open.back();
            open.pop_back();
            for (int a : treeOf[n]) {
                const int other = arcs[a].tail == n ? arcs[a].head : arcs[a].tail;
                if (seen[other]) continue;
                seen[other] = 1;
                rank[other] = other == arcs[a].head ? rank[n] + 1 : rank[n] - 1;
                open.push_back(other);
            }
        }
    };

    // Swap a tree arc whose cut value is negative for the tightest arc the other way across.
    const int limit = std::max(200, 4 * count);
    for (int iteration = 0; iteration < limit; ++iteration) {
        number();
        int leave = -1;
        float worst = -1e-4f;
        for (int a = 0; a < m; ++a) {
            if (!treeArc[a]) continue;
            const auto inTail = tailSide(a);
            float cut = 0.0f;
            for (int f = 0; f < m; ++f) {
                const bool tail = inTail(arcs[f].tail);
                const bool head = inTail(arcs[f].head);
                if (tail && !head) cut += weight[f];
                if (!tail && head) cut -= weight[f];
            }
            if (cut < worst) {
                worst = cut;
                leave = a;
            }
        }
        if (leave < 0) break;
        const auto inTail = tailSide(leave);
        int enter = -1;
        int least = INT_MAX;
        for (int f = 0; f < m; ++f) {
            if (treeArc[f] || inTail(arcs[f].tail) || !inTail(arcs[f].head)) continue;
            if (slack(f) < least) {
                least = slack(f);
                enter = f;
            }
        }
        if (enter < 0) break;
        treeArc[leave] = 0;
        treeArc[enter] = 1;
        for (auto& arcsOf : treeOf) arcsOf.clear();
        for (int a = 0; a < m; ++a) {
            if (!treeArc[a]) continue;
            treeOf[arcs[a].tail].push_back(a);
            treeOf[arcs[a].head].push_back(a);
        }
        reRank();
    }
    // Where it makes no difference to the length, as far right as it goes: next to what it
    // feeds, as a texture is next to the surface it's read into. From the right, so what's
    // moved makes room for what feeds it.
    std::vector<float> into(count, 0.0f), outOf(count, 0.0f);
    for (int a = 0; a < m; ++a) {
        outOf[arcs[a].tail] += weight[a];
        into[arcs[a].head] += weight[a];
    }
    std::vector<int> byRank(count);
    std::iota(byRank.begin(), byRank.end(), 0);
    std::stable_sort(byRank.begin(), byRank.end(), [&rank](int a, int b) { return rank[a] > rank[b]; });
    for (int n : byRank) {
        if (outOf[n] == 0.0f || into[n] > outOf[n]) continue;
        int furthest = INT_MAX;
        for (int a : incident[n]) {
            if (arcs[a].tail == n) furthest = std::min(furthest, rank[arcs[a].head] - 1);
        }
        rank[n] = std::max(rank[n], furthest);
    }
    const int least = *std::min_element(rank.begin(), rank.end());
    for (int& r : rank) r -= least;
    return rank;
}

// ---- 3. to 6. One connected part

// A node, or a point of a line passing through a column.
struct Item {
    int node = -1;  // the part's node, or -1 for a line's point
    int rank = 0;
    float width = 0.0f;
    float height = 0.0f;
};

// A line between neighbouring columns.
struct Segment {
    int left = 0;
    int right = 0;
    float leftY = 0.0f;  // down from left's top
    float rightY = 0.0f;
};

struct PartLayout {
    std::vector<LayeredPoint> positions;  // the part's nodes'
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

class Part {
public:
    Part(const std::vector<LayeredNode>& nodes, const std::vector<Arc>& arcs, const LayeredLayoutOptions& options)
        : m_nodes(nodes), m_arcs(arcs), m_options(options) {}

    PartLayout run() {
        rankNodes();
        buildItems();
        order();
        placeHeights();
        return place();
    }

private:
    void rankNodes() {
        const int count = static_cast<int>(m_nodes.size());
        std::vector<int> first;
        for (int n = 0; n < count; ++n) {
            if (m_nodes[n].side == LayeredNode::Side::First) first.push_back(n);
        }
        const std::vector<bool> turn = loopBreakers(count, m_arcs, first);
        m_dag = m_arcs;
        for (std::size_t a = 0; a < m_dag.size(); ++a) {
            if (!turn[a]) continue;
            // Turned round, it's drawn going back: its ends are the nodes' middles.
            Arc& arc = m_dag[a];
            std::swap(arc.tail, arc.head);
            arc.tailY = m_nodes[arc.tail].height / 2.0f;
            arc.headY = m_nodes[arc.head].height / 2.0f;
        }
        std::vector<float> weight(m_dag.size(), 1.0f);
        m_rank = networkSimplexRanks(count, m_dag, weight);

        // First and Last nodes in columns of their own, at either end.
        int lowest = INT_MAX;
        int highest = INT_MIN;
        for (int n = 0; n < count; ++n) {
            if (m_nodes[n].side != LayeredNode::Side::Free) continue;
            lowest = std::min(lowest, m_rank[n]);
            highest = std::max(highest, m_rank[n]);
        }
        if (lowest == INT_MAX) lowest = highest = 0;
        for (int n = 0; n < count; ++n) {
            if (m_nodes[n].side == LayeredNode::Side::First) m_rank[n] = lowest - 1;
            if (m_nodes[n].side == LayeredNode::Side::Last) m_rank[n] = highest + 1;
        }
        // A Last node a turned arc leaves would have it going left; it stays where it is
        // and so does that arc's other end, turned.
        for (Arc& arc : m_dag) {
            if (m_rank[arc.head] <= m_rank[arc.tail]) {
                std::swap(arc.tail, arc.head);
                std::swap(arc.tailY, arc.headY);
            }
        }
        const int least = *std::min_element(m_rank.begin(), m_rank.end());
        for (int& r : m_rank) r -= least;
    }

    void buildItems() {
        for (std::size_t n = 0; n < m_nodes.size(); ++n) {
            m_items.push_back(Item{static_cast<int>(n), m_rank[n], m_nodes[n].width, m_nodes[n].height});
        }
        for (const Arc& arc : m_dag) {
            if (m_rank[arc.head] == m_rank[arc.tail]) continue;  // only First and Last together
            Line line{arc.tail, arc.head, arc.tailY, arc.headY, {}};
            int from = arc.tail;
            float fromY = arc.tailY;
            for (int r = m_rank[arc.tail] + 1; r < m_rank[arc.head]; ++r) {
                const int point = static_cast<int>(m_items.size());
                m_items.push_back(Item{-1, r, 0.0f, 0.0f});
                addSegment(from, fromY, point, 0.0f);
                line.points.push_back(point);
                from = point;
                fromY = 0.0f;
            }
            addSegment(from, fromY, arc.head, arc.headY);
            m_lines.push_back(std::move(line));
        }
        m_linesOf.assign(m_items.size(), {});
        m_pointOf.assign(m_items.size(), {-1, 0});
        for (std::size_t l = 0; l < m_lines.size(); ++l) {
            m_linesOf[m_lines[l].tail].push_back(static_cast<int>(l));
            m_linesOf[m_lines[l].head].push_back(static_cast<int>(l));
            for (std::size_t k = 0; k < m_lines[l].points.size(); ++k) m_pointOf[m_lines[l].points[k]] = {static_cast<int>(l), static_cast<int>(k)};
        }
        int columns = 0;
        for (const Item& item : m_items) columns = std::max(columns, item.rank + 1);
        m_layers.assign(columns, {});
        m_leftOf.assign(m_items.size(), {});
        m_rightOf.assign(m_items.size(), {});
        for (std::size_t s = 0; s < m_segments.size(); ++s) {
            m_rightOf[m_segments[s].left].push_back(static_cast<int>(s));
            m_leftOf[m_segments[s].right].push_back(static_cast<int>(s));
        }
    }

    void addSegment(int left, float leftY, int right, float rightY) { m_segments.push_back(Segment{left, right, leftY, rightY}); }

    // ---- 4. Order

    void order() {
        // To start: back from the last column, in the order things reach it, port by port.
        const int columns = static_cast<int>(m_layers.size());
        std::vector<char> placed(m_items.size(), 0);
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].rank == columns - 1) {
                m_layers[columns - 1].push_back(static_cast<int>(i));
                placed[i] = 1;
            }
        }
        for (int r = columns - 2; r >= 0; --r) {
            for (int item : m_layers[r + 1]) {
                std::vector<int> in = m_leftOf[item];
                std::stable_sort(in.begin(), in.end(), [this](int a, int b) { return m_segments[a].rightY < m_segments[b].rightY; });
                for (int s : in) {
                    const int left = m_segments[s].left;
                    if (placed[left]) continue;
                    placed[left] = 1;
                    m_layers[r].push_back(left);
                }
            }
            for (std::size_t i = 0; i < m_items.size(); ++i) {
                if (m_items[i].rank == r && !placed[i]) {
                    placed[i] = 1;
                    m_layers[r].push_back(static_cast<int>(i));
                }
            }
        }
        m_position.assign(m_items.size(), 0);
        updatePositions();

        std::vector<std::vector<int>> best = m_layers;
        int fewest = crossings();
        for (int sweep = 0; sweep < 24 && fewest > 0; ++sweep) {
            if (sweep % 2 == 0) {
                for (int r = 1; r < columns; ++r) sortBy(r, r - 1);
            } else {
                for (int r = columns - 2; r >= 0; --r) sortBy(r, r + 1);
            }
            transpose();
            const int now = crossings();
            if (now < fewest) {
                fewest = now;
                best = m_layers;
            }
        }
        m_layers = std::move(best);
        updatePositions();
    }

    void updatePositions() {
        for (const auto& layer : m_layers) {
            for (std::size_t i = 0; i < layer.size(); ++i) m_position[layer[i]] = static_cast<int>(i);
        }
    }

    float separation(int above, int below) const {
        // Lines are drawn straight, not through their points, so points next to each other
        // needn't be apart: they only keep what's next to them clear of the line.
        const bool aboveLine = m_items[above].node < 0;
        const bool belowLine = m_items[below].node < 0;
        if (aboveLine && belowLine) return 1.0f;
        return m_items[above].height + (aboveLine || belowLine ? m_options.lineGap : m_options.rowGap);
    }

    // Layer r in the order of where its lines meet layer `fixed`, stacked as it is: each
    // item's middle, were it placed where they pull it. What has no lines there keeps its place.
    void sortBy(int r, int fixed) {
        std::vector<float> top(m_items.size(), 0.0f);
        float y = 0.0f;
        for (std::size_t i = 0; i < m_layers[fixed].size(); ++i) {
            const int item = m_layers[fixed][i];
            top[item] = y;
            if (i + 1 < m_layers[fixed].size()) y += separation(item, m_layers[fixed][i + 1]);
        }
        std::vector<int>& layer = m_layers[r];
        std::vector<std::pair<float, int>> sortable;
        std::vector<char> kept(layer.size(), 0);
        for (std::size_t i = 0; i < layer.size(); ++i) {
            const int item = layer[i];
            const bool fromLeft = fixed < r;
            const std::vector<int>& segments = fromLeft ? m_leftOf[item] : m_rightOf[item];
            if (segments.empty()) {
                kept[i] = 1;
                continue;
            }
            float total = 0.0f;
            for (int s : segments) {
                const Segment& segment = m_segments[s];
                const float there = fromLeft ? top[segment.left] + segment.leftY : top[segment.right] + segment.rightY;
                const float here = fromLeft ? segment.rightY : segment.leftY;
                total += there - here;
            }
            sortable.emplace_back(total / static_cast<float>(segments.size()) + m_items[item].height / 2.0f, item);
        }
        std::stable_sort(sortable.begin(), sortable.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<int> sorted(layer.size());
        std::size_t next = 0;
        for (std::size_t i = 0; i < layer.size(); ++i) sorted[i] = kept[i] ? layer[i] : sortable[next++].second;
        layer = std::move(sorted);
        for (std::size_t i = 0; i < layer.size(); ++i) m_position[layer[i]] = static_cast<int>(i);
    }

    // Where a segment's far end is, for comparing with another's: its item, then its port.
    std::pair<int, float> farEnd(int s, bool leftward) const {
        const Segment& segment = m_segments[s];
        return leftward ? std::pair<int, float>(m_position[segment.left], segment.leftY) : std::pair<int, float>(m_position[segment.right], segment.rightY);
    }

    // How many of `upper`'s and `lower`'s lines cross each other, with upper right above lower.
    int pairCrossings(int upper, int lower) const {
        int count = 0;
        for (const bool leftward : {true, false}) {
            const std::vector<int>& ups = leftward ? m_leftOf[upper] : m_rightOf[upper];
            const std::vector<int>& downs = leftward ? m_leftOf[lower] : m_rightOf[lower];
            for (int a : ups) {
                for (int b : downs) {
                    if (farEnd(a, leftward) > farEnd(b, leftward)) ++count;
                }
            }
        }
        return count;
    }

    // Swaps neighbours wherever that crosses fewer lines, until none does.
    void transpose() {
        for (int pass = 0; pass < 8; ++pass) {
            bool better = false;
            for (auto& layer : m_layers) {
                for (std::size_t i = 0; i + 1 < layer.size(); ++i) {
                    if (pairCrossings(layer[i + 1], layer[i]) < pairCrossings(layer[i], layer[i + 1])) {
                        std::swap(layer[i], layer[i + 1]);
                        m_position[layer[i]] = static_cast<int>(i);
                        m_position[layer[i + 1]] = static_cast<int>(i + 1);
                        better = true;
                    }
                }
            }
            if (!better) break;
        }
    }

    int crossings() const {
        // Between each pair of neighbouring columns, lines whose ends are in opposite orders.
        std::vector<std::vector<int>> between(m_layers.size());
        for (std::size_t s = 0; s < m_segments.size(); ++s) between[m_items[m_segments[s].left].rank].push_back(static_cast<int>(s));
        int count = 0;
        for (const auto& segments : between) {
            for (std::size_t a = 0; a < segments.size(); ++a) {
                for (std::size_t b = a + 1; b < segments.size(); ++b) {
                    const auto leftA = farEnd(segments[a], true);
                    const auto leftB = farEnd(segments[b], true);
                    const auto rightA = farEnd(segments[a], false);
                    const auto rightB = farEnd(segments[b], false);
                    if ((leftA < leftB && rightA > rightB) || (leftA > leftB && rightA < rightB)) ++count;
                }
            }
        }
        return count;
    }

    // ---- 5. Heights

    void placeHeights() {
        m_y.assign(m_items.size(), 0.0f);
        for (const auto& layer : m_layers) {
            float y = 0.0f;
            for (std::size_t i = 0; i < layer.size(); ++i) {
                m_y[layer[i]] = y;
                if (i + 1 < layer.size()) y += separation(layer[i], layer[i + 1]);
            }
            for (int item : layer) m_y[item] -= y / 2.0f;
        }
        // Least squares first, then a line that's far off counts less (Huber's loss, by
        // reweighting), which straightens most lines fully rather than all a bit. Not all the
        // way to the least absolute distances: a node pulled equally both ways could go anywhere.
        const int columns = static_cast<int>(m_layers.size());
        for (int sweep = 0; sweep < 400; ++sweep) {
            const bool straighten = sweep >= 100;
            float moved = 0.0f;
            for (int step = 0; step < columns; ++step) {
                const int r = sweep % 2 == 0 ? step : columns - 1 - step;
                moved = std::max(moved, placeLayer(m_layers[r], straighten));
            }
            if (moved < 0.01f && sweep < 100) sweep = 99;
            else if (moved < 0.01f) break;
        }
    }

    // Puts a layer as near to where its lines pull each item as its order and spacing let it:
    // with each item's offset from the top taken off, a weighted isotonic regression, solved
    // by pooling neighbours that are the wrong way round. How far the furthest moved.
    float placeLayer(const std::vector<int>& layer, bool straighten) {
        if (layer.empty()) return 0.0f;
        struct Block {
            double weight = 0.0;
            double sum = 0.0;
            std::size_t count = 0;
            double value() const { return sum / weight; }
        };
        std::vector<Block> blocks;
        std::vector<float> offset(layer.size(), 0.0f);
        for (std::size_t i = 0; i < layer.size(); ++i) {
            if (i > 0) offset[i] = offset[i - 1] + separation(layer[i - 1], layer[i]);
            const int item = layer[i];
            double weight = 0.0;
            double sum = 0.0;
            // Wanting to be at y, this much.
            const auto want = [&](double y, double w) {
                weight += w;
                sum += w * y;
            };
            if (m_pointOf[item].first >= 0) {
                // A line's point: on the straight line between its ends, as it's drawn.
                const Line& line = m_lines[m_pointOf[item].first];
                const double t = along(line, m_pointOf[item].second);
                const double on = (1.0 - t) * (m_y[line.tail] + line.tailY) + t * (m_y[line.head] + line.headY);
                want(on, onTheLine(m_y[item] - on));
            }
            for (int l : m_linesOf[item]) {
                const Line& line = m_lines[l];
                const bool isTail = line.tail == item;
                const double here = isTail ? line.tailY : line.headY;
                const double there = isTail ? m_y[line.head] + line.headY : m_y[line.tail] + line.tailY;
                // Level with the other end...
                double w = 1.0;
                if (straighten) w = std::max(0.5, 1.0 / std::max(1.0, std::abs(m_y[item] + here - there) / 20.0));
                want(there - here, w);
                // ...and where its points are, so the line keeps clear of what they keep clear of.
                for (std::size_t k = 0; k < line.points.size(); ++k) {
                    const double t = along(line, static_cast<int>(k));
                    const double share = isTail ? 1.0 - t : t;
                    const double point = m_y[line.points[k]];
                    const double on = share * (m_y[item] + here) + (1.0 - share) * there;
                    want((point - (1.0 - share) * there) / share - here, onTheLine(point - on) * share * share);
                }
            }
            if (weight == 0.0) {
                weight = 1e-3;
                sum = weight * m_y[item];
            }
            blocks.push_back(Block{weight, sum - weight * offset[i], 1});
            while (blocks.size() > 1 && blocks[blocks.size() - 2].value() > blocks.back().value()) {
                const Block last = blocks.back();
                blocks.pop_back();
                blocks.back().weight += last.weight;
                blocks.back().sum += last.sum;
                blocks.back().count += last.count;
            }
        }
        float moved = 0.0f;
        std::size_t i = 0;
        for (const Block& block : blocks) {
            const double value = block.value();
            for (std::size_t k = 0; k < block.count; ++k, ++i) {
                const float y = static_cast<float>(value) + offset[i];
                moved = std::max(moved, std::abs(y - m_y[layer[i]]));
                m_y[layer[i]] = y;
            }
        }
        return moved;
    }

    // ---- 6. Columns' x, and the result

    PartLayout place() const {
        const int columns = static_cast<int>(m_layers.size());
        std::vector<float> width(columns, 0.0f);
        for (const Item& item : m_items) width[item.rank] = std::max(width[item.rank], item.width);
        std::vector<float> steepest(columns, 0.0f);
        for (const Segment& segment : m_segments) {
            const float rise = std::abs((m_y[segment.left] + segment.leftY) - (m_y[segment.right] + segment.rightY));
            const int r = m_items[segment.left].rank;
            steepest[r] = std::max(steepest[r], rise);
        }
        std::vector<float> x(columns, 0.0f);
        for (int r = 1; r < columns; ++r) {
            // More room for steep lines, so they don't come out as walls.
            const float gap = m_options.columnGap + std::min(160.0f, 0.12f * steepest[r - 1]);
            x[r] = x[r - 1] + width[r - 1] + gap;
        }
        PartLayout layout;
        layout.positions.resize(m_nodes.size());
        layout.left = layout.top = 1e30f;
        layout.right = layout.bottom = -1e30f;
        for (std::size_t n = 0; n < m_nodes.size(); ++n) {
            const Item& item = m_items[n];
            const LayeredPoint at{x[item.rank] + (width[item.rank] - item.width) / 2.0f, m_y[n]};
            layout.positions[n] = at;
            layout.left = std::min(layout.left, at.x);
            layout.top = std::min(layout.top, at.y);
            layout.right = std::max(layout.right, at.x + item.width);
            layout.bottom = std::max(layout.bottom, at.y + item.height);
        }
        return layout;
    }

    const std::vector<LayeredNode>& m_nodes;
    const std::vector<Arc>& m_arcs;
    const LayeredLayoutOptions& m_options;
    std::vector<Arc> m_dag;
    std::vector<int> m_rank;
    std::vector<Item> m_items;
    std::vector<Segment> m_segments;
    // A line from tail to head, with a point in each column it passes through.
    struct Line {
        int tail = 0;
        int head = 0;
        float tailY = 0.0f;
        float headY = 0.0f;
        std::vector<int> points;
    };
    // How far along a line its kth point is.
    static double along(const Line& line, int k) { return (k + 1.0) / (static_cast<double>(line.points.size()) + 1.0); }
    // How much a line's point wants to be on it, `off` away: more than the line wants to be
    // level, so what it passes keeps clear of it, but less the further off it is (Huber's loss),
    // so a node's not dragged far away for a line to miss another.
    static double onTheLine(double off) { return 4.0 / std::max(1.0, std::abs(off) / 60.0); }
    std::vector<Line> m_lines;
    std::vector<std::vector<int>> m_linesOf;          // lines an item is an end of
    std::vector<std::pair<int, int>> m_pointOf;       // a point's line and which of its points
    std::vector<std::vector<int>> m_leftOf;   // segments ending at an item
    std::vector<std::vector<int>> m_rightOf;  // segments starting at one
    std::vector<std::vector<int>> m_layers;
    std::vector<int> m_position;
    std::vector<float> m_y;
};

float portY(const std::vector<float>& ports, std::size_t port, float height) { return port < ports.size() ? ports[port] : height / 2.0f; }

int findRoot(std::vector<int>& parent, int n) {
    while (parent[n] != n) n = parent[n] = parent[parent[n]];
    return n;
}

}  // namespace

std::vector<LayeredPoint> layeredLayout(const std::vector<LayeredNode>& nodes, const std::vector<LayeredEdge>& edges, const LayeredLayoutOptions& options) {
    const int count = static_cast<int>(nodes.size());
    std::vector<LayeredPoint> result(nodes.size());
    if (count == 0) return result;

    // Parts that aren't connected, with what's connected to itself only not counting.
    std::vector<int> parent(count);
    std::iota(parent.begin(), parent.end(), 0);
    std::vector<LayeredEdge> lines;
    for (const LayeredEdge& edge : edges) {
        if (edge.from >= nodes.size() || edge.to >= nodes.size() || edge.from == edge.to) continue;
        lines.push_back(edge);
        parent[findRoot(parent, static_cast<int>(edge.from))] = findRoot(parent, static_cast<int>(edge.to));
    }
    std::vector<std::vector<int>> parts;
    std::vector<int> partOf(count, -1);
    for (int n = 0; n < count; ++n) {
        const int root = findRoot(parent, n);
        if (partOf[root] < 0) {
            partOf[root] = static_cast<int>(parts.size());
            parts.emplace_back();
        }
        parts[partOf[root]].push_back(n);
    }
    std::vector<int> alone;
    std::vector<std::vector<int>> connected;
    for (auto& part : parts) {
        if (part.size() == 1) {
            alone.push_back(part[0]);
        } else {
            connected.push_back(std::move(part));
        }
    }
    // The part ending in a Last node first, such as a Material's, then the biggest.
    const auto hasLast = [&nodes](const std::vector<int>& part) {
        return std::any_of(part.begin(), part.end(), [&nodes](int n) { return nodes[n].side == LayeredNode::Side::Last; });
    };
    std::stable_sort(connected.begin(), connected.end(), [&](const auto& a, const auto& b) {
        if (hasLast(a) != hasLast(b)) return hasLast(a);
        return a.size() > b.size();
    });

    float right = 0.0f;
    float bottom = 0.0f;
    bool any = false;
    float left = 0.0f;
    for (const std::vector<int>& part : connected) {
        std::vector<int> local(count, -1);
        std::vector<LayeredNode> partNodes;
        for (int n : part) {
            local[n] = static_cast<int>(partNodes.size());
            partNodes.push_back(nodes[n]);
        }
        std::vector<Arc> arcs;
        for (const LayeredEdge& edge : lines) {
            const int from = local[edge.from];
            const int to = local[edge.to];
            if (from < 0 || to < 0) continue;
            arcs.push_back(Arc{from, to, portY(nodes[edge.from].out, edge.fromPort, nodes[edge.from].height),
                               portY(nodes[edge.to].in, edge.toPort, nodes[edge.to].height)});
        }
        const PartLayout layout = Part(partNodes, arcs, options).run();
        // The first from x = 0, centred on y = 0; the others under it, lined up on the right.
        float dx = 0.0f;
        float dy = 0.0f;
        if (!any) {
            dx = -layout.left;
            dy = -(layout.top + layout.bottom) / 2.0f;
            left = 0.0f;
            right = layout.right + dx;
        } else {
            dx = right - layout.right;
            dy = bottom + options.partGap - layout.top;
            left = std::min(left, layout.left + dx);
        }
        bottom = layout.bottom + dy;
        any = true;
        for (std::size_t i = 0; i < part.size(); ++i) result[part[i]] = LayeredPoint{layout.positions[i].x + dx, layout.positions[i].y + dy};
    }

    // What's connected to nothing: in rows underneath, First ones first and Last ones last.
    std::stable_sort(alone.begin(), alone.end(), [&nodes](int a, int b) { return static_cast<int>(nodes[a].side) < static_cast<int>(nodes[b].side); });
    const float rowWidth = std::max(right - left, 900.0f);
    float x = left;
    float y = any ? bottom + options.partGap : 0.0f;
    float rowHeight = 0.0f;
    for (int n : alone) {
        if (x > left && x + nodes[n].width > left + rowWidth) {
            x = left;
            y += rowHeight + options.rowGap;
            rowHeight = 0.0f;
        }
        result[n] = LayeredPoint{x, y};
        x += nodes[n].width + options.columnGap / 2.0f;
        rowHeight = std::max(rowHeight, nodes[n].height);
    }
    return result;
}

namespace {

struct Line {
    float x0, y0, x1, y1;
};

float cross(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

bool segmentsCross(const Line& a, const Line& b) {
    const float d1 = cross(a.x1 - a.x0, a.y1 - a.y0, b.x0 - a.x0, b.y0 - a.y0);
    const float d2 = cross(a.x1 - a.x0, a.y1 - a.y0, b.x1 - a.x0, b.y1 - a.y0);
    const float d3 = cross(b.x1 - b.x0, b.y1 - b.y0, a.x0 - b.x0, a.y0 - b.y0);
    const float d4 = cross(b.x1 - b.x0, b.y1 - b.y0, a.x1 - b.x0, a.y1 - b.y0);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

// Liang–Barsky: whether a segment passes through a box's inside.
bool segmentHitsBox(const Line& line, float left, float top, float right, float bottom) {
    float t0 = 0.0f;
    float t1 = 1.0f;
    const float dx = line.x1 - line.x0;
    const float dy = line.y1 - line.y0;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {line.x0 - left, right - line.x0, line.y0 - top, bottom - line.y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] <= 0.0f) return false;
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.0f) {
            t0 = std::max(t0, t);
        } else {
            t1 = std::min(t1, t);
        }
        if (t0 >= t1) return false;
    }
    return true;
}

}  // namespace

LayeredLayoutQuality layeredLayoutQuality(const std::vector<LayeredNode>& nodes, const std::vector<LayeredEdge>& edges, const std::vector<LayeredPoint>& positions) {
    LayeredLayoutQuality quality;
    std::vector<Line> lines;
    std::vector<const LayeredEdge*> of;
    for (const LayeredEdge& edge : edges) {
        if (edge.from >= nodes.size() || edge.to >= nodes.size() || edge.from == edge.to) continue;
        const LayeredNode& from = nodes[edge.from];
        const LayeredNode& to = nodes[edge.to];
        lines.push_back(Line{positions[edge.from].x + from.width, positions[edge.from].y + portY(from.out, edge.fromPort, from.height), positions[edge.to].x,
                             positions[edge.to].y + portY(to.in, edge.toPort, to.height)});
        of.push_back(&edge);
        quality.length += std::hypot(lines.back().x1 - lines.back().x0, lines.back().y1 - lines.back().y0);
    }
    for (std::size_t a = 0; a < lines.size(); ++a) {
        for (std::size_t b = a + 1; b < lines.size(); ++b) {
            const bool sameStart = of[a]->from == of[b]->from && of[a]->fromPort == of[b]->fromPort;
            const bool sameEnd = of[a]->to == of[b]->to && of[a]->toPort == of[b]->toPort;
            if (!sameStart && !sameEnd && segmentsCross(lines[a], lines[b])) ++quality.crossings;
        }
        for (std::size_t n = 0; n < nodes.size(); ++n) {
            if (n == of[a]->from || n == of[a]->to) continue;
            const LayeredPoint& at = positions[n];
            if (segmentHitsBox(lines[a], at.x + 1.0f, at.y + 1.0f, at.x + nodes[n].width - 1.0f, at.y + nodes[n].height - 1.0f)) ++quality.throughNodes;
        }
    }
    for (std::size_t a = 0; a < nodes.size(); ++a) {
        for (std::size_t b = a + 1; b < nodes.size(); ++b) {
            const LayeredPoint& p = positions[a];
            const LayeredPoint& q = positions[b];
            if (p.x < q.x + nodes[b].width - 0.5f && q.x < p.x + nodes[a].width - 0.5f && p.y < q.y + nodes[b].height - 0.5f && q.y < p.y + nodes[a].height - 0.5f) {
                ++quality.overlaps;
            }
        }
    }
    return quality;
}

}  // namespace usdcc::usd
