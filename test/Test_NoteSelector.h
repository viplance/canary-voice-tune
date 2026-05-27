#pragma once

// Verifies NoteSelector::chooseActiveNote: the detected pitch must snap to the
// mathematically closest ENABLED key (symmetric, no above/below bias), and the
// incumbent must not flicker when the pitch jitters around the switch midpoint.
void runNoteSelectorTests();
