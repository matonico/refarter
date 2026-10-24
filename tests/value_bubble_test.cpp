#include "value_bubble.h"
#include <iostream>

using namespace refarter;

int main() {
    const RECT work{0, 0, 1920, 1080};
    const SIZE size{50, 26};
    // Centred above the knob, with the gap between the bubble and the anchor.
    auto at = placeValueBubble({500, 400, 512, 424}, size, 4, work);
    if (at.x != 481 || at.y != 400 - 4 - 26) { std::cerr << "not centred above the knob\n"; return 1; }
    // Clamped at both ends of the work area.
    at = placeValueBubble({0, 400, 12, 424}, size, 4, work);
    if (at.x != 0) { std::cerr << "left edge not clamped\n"; return 1; }
    at = placeValueBubble({1908, 400, 1920, 424}, size, 4, work);
    if (at.x != 1920 - 50) { std::cerr << "right edge not clamped\n"; return 1; }
    // No room above: below the anchor, never over it.
    at = placeValueBubble({500, 10, 512, 34}, size, 4, work);
    if (at.y != 34 + 4) { std::cerr << "no flip below\n"; return 1; }
    // A work area narrower than the bubble keeps its left edge.
    at = placeValueBubble({10, 400, 22, 424}, size, 4, {0, 0, 30, 1080});
    if (at.x != 0) { std::cerr << "narrow work area\n"; return 1; }
    // The gate's problems: a level, a time, or the close level over the open one (both fields).
    auto problem = gateProblem(1, false, -96, 0, 10000);
    if (problem.text != L"enter −96 to 0 db" || problem.first != 1 || problem.last != 1) { std::cerr << "level problem\n"; return 1; }
    problem = gateProblem(3, false, -96, 0, 10000);
    if (problem.text != L"enter 0–10000 ms" || problem.first != 3 || problem.last != 3) { std::cerr << "time problem\n"; return 1; }
    problem = gateProblem(0, true, -96, 0, 10000);
    if (problem.text != L"close must be at or below open" || problem.first != 0 || problem.last != 1) { std::cerr << "order problem\n"; return 1; }
    problem = gateProblem(1, true, -96, 0, 10000);
    if (problem.first != 0 || problem.last != 1) { std::cerr << "order problem spans the pair\n"; return 1; }
    return 0;
}
