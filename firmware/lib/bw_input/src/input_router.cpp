#include <bw/input/input_router.h>

namespace bw::input {

bool InputRouter::route(InputContextId context, const GestureRecord& record, Command& out) const {
    for (const MapCell& cell : kInputMap) {
        if (cell.context != context || cell.gesture != record.key) {
            continue;
        }
        if (!cell.has_command) {
            return false;
        }
        out = Command{cell.type, record.t_ms, kCommandSourceButton, cell.arg0, cell.arg1};
        return true;
    }
    return false;
}

}  // namespace bw::input
