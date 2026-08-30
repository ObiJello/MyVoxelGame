// File: src/common/world/portal/BlockUtil.cpp
//
// Line references are to
// minecraft_code/decompiled_net/minecraft/util/BlockUtil.java.

#include "BlockUtil.hpp"

#include <utility>
#include <vector>

namespace Game {

    namespace {

        struct IntBounds {
            int min = 0;
            int max = 0;
        };

        inline glm::ivec3 Relative(const glm::ivec3& p, Direction d, int n) {
            return { p.x + StepX(d) * n, p.y + StepY(d) * n, p.z + StepZ(d) * n };
        }

        // Direction.get(AxisDirection.NEGATIVE, axis)
        constexpr Direction NegativeOn(Axis a) {
            switch (a) {
                case Axis::X: return Direction::West;
                case Axis::Y: return Direction::Down;
                default:      return Direction::North;
            }
        }

        // BlockUtil.java:68 — how many consecutive matching cells lie in
        // `direction` from `pos`, not counting `pos` itself, capped at `limit`.
        int GetLimit(const std::function<bool(const glm::ivec3&)>& test,
                     const glm::ivec3& pos, Direction direction, int limit)
        {
            int n = 0;
            glm::ivec3 cursor = pos;
            while (n < limit) {
                cursor = Relative(cursor, direction, 1);
                if (!test(cursor)) break;
                ++n;
            }
            return n;
        }

        // BlockUtil.java:77 — largest rectangle in a histogram, monotonic
        // stack. Returns ((startColumn, endColumn), height); the column range
        // is inclusive.
        std::pair<IntBounds, int> GetMaxRectangleLocation(const std::vector<int>& columns) {
            int maxStart = 0;
            int maxEnd = 0;
            int maxHeight = 0;

            std::vector<int> stack;
            stack.push_back(0);

            const int n = static_cast<int>(columns.size());
            for (int column = 1; column <= n; ++column) {
                const int height = (column == n) ? 0 : columns[column];

                while (!stack.empty()) {
                    const int stackHeight = columns[stack.back()];
                    if (height >= stackHeight) {
                        stack.push_back(column);
                        break;
                    }
                    stack.pop_back();
                    const int start = stack.empty() ? 0 : stack.back() + 1;
                    if (stackHeight * (column - start) > maxHeight * (maxEnd - maxStart)) {
                        maxEnd = column;
                        maxStart = start;
                        maxHeight = stackHeight;
                    }
                }

                if (stack.empty()) stack.push_back(column);
            }

            return { IntBounds{ maxStart, maxEnd - 1 }, maxHeight };
        }

    } // namespace

    // BlockUtil.java:16
    FoundRectangle GetLargestRectangleAround(
        const glm::ivec3& center,
        Axis axis1, int limit1,
        Axis axis2, int limit2,
        const std::function<bool(const glm::ivec3&)>& test)
    {
        const Direction negative1 = NegativeOn(axis1);
        const Direction positive1 = Opposite(negative1);
        const Direction negative2 = NegativeOn(axis2);
        const Direction positive2 = Opposite(negative2);

        // How far the matching region reaches along axis1 from the center.
        const int negativeDelta1 = GetLimit(test, center, negative1, limit1);
        const int positiveDelta1 = GetLimit(test, center, positive1, limit1);
        const int centerIndex1   = negativeDelta1;

        // For each column along axis1, how far it reaches along axis2. Each
        // column is bounded by its NEIGHBOUR's reach, not by `limit2` — that
        // is what keeps the collected shape convex, so the histogram pass
        // below is measuring a real rectangle candidate rather than a
        // staircase that never existed.
        std::vector<IntBounds> boundsByAxis1(
            static_cast<size_t>(negativeDelta1 + 1 + positiveDelta1));

        boundsByAxis1[static_cast<size_t>(centerIndex1)] = IntBounds{
            GetLimit(test, center, negative2, limit2),
            GetLimit(test, center, positive2, limit2)
        };
        const int centerIndex2 = boundsByAxis1[static_cast<size_t>(centerIndex1)].min;

        for (int i = 1; i <= negativeDelta1; ++i) {
            const IntBounds& last = boundsByAxis1[static_cast<size_t>(centerIndex1 - (i - 1))];
            const glm::ivec3 at = Relative(center, negative1, i);
            boundsByAxis1[static_cast<size_t>(centerIndex1 - i)] = IntBounds{
                GetLimit(test, at, negative2, last.min),
                GetLimit(test, at, positive2, last.max)
            };
        }
        for (int i = 1; i <= positiveDelta1; ++i) {
            const IntBounds& last = boundsByAxis1[static_cast<size_t>(centerIndex1 + i - 1)];
            const glm::ivec3 at = Relative(center, positive1, i);
            boundsByAxis1[static_cast<size_t>(centerIndex1 + i)] = IntBounds{
                GetLimit(test, at, negative2, last.min),
                GetLimit(test, at, positive2, last.max)
            };
        }

        int minAxis1  = 0;
        int minAxis2  = 0;
        int sizeAxis1 = 0;
        int sizeAxis2 = 0;
        std::vector<int> columns(boundsByAxis1.size(), 0);

        // Sweep the bottom edge downward from the center row; for each choice
        // of bottom, the columns form a histogram and the best rectangle in it
        // is one stack pass away. The best over all bottoms is the answer.
        for (int i2 = centerIndex2; i2 >= 0; --i2) {
            for (size_t i1 = 0; i1 < boundsByAxis1.size(); ++i1) {
                const IntBounds& b2 = boundsByAxis1[i1];
                const int min2 = centerIndex2 - b2.min;
                const int max2 = centerIndex2 + b2.max;
                columns[i1] = (i2 >= min2 && i2 <= max2) ? (max2 + 1 - i2) : 0;
            }

            auto [boundsAxis1, newSizeAxis2] = GetMaxRectangleLocation(columns);
            const int newSizeAxis1 = 1 + boundsAxis1.max - boundsAxis1.min;

            if (newSizeAxis1 * newSizeAxis2 > sizeAxis1 * sizeAxis2) {
                minAxis1  = boundsAxis1.min;
                minAxis2  = i2;
                sizeAxis1 = newSizeAxis1;
                sizeAxis2 = newSizeAxis2;
            }
        }

        // MC: center.relative(axis1, minAxis1 - centerIndex1)
        //           .relative(axis2, minAxis2 - centerIndex2)
        // Direction.relative(Axis, n) steps in the POSITIVE direction of the
        // axis for positive n, so both deltas apply on the positive side.
        glm::ivec3 corner = Relative(center, Opposite(NegativeOn(axis1)),
                                     minAxis1 - centerIndex1);
        corner = Relative(corner, Opposite(NegativeOn(axis2)),
                          minAxis2 - centerIndex2);

        return FoundRectangle{ corner, sizeAxis1, sizeAxis2 };
    }

} // namespace Game
