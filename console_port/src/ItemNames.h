#pragma once

namespace console {

// English display name from the archived console registrations and strings.
// Returns nullptr for IDs absent from the archived creative catalog.
const char* consoleSourceItemName(int id, int damage);

}
