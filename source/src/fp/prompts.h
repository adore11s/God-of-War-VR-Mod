#pragma once
namespace gowvr::fp {
// D-312 (owner: "the message 'Press [button] to equip the Leviathan Axe' should say 'Reach over your right shoulder to equip
// the Leviathan Axe' - only in first person with motion controls on"). The game's text table (STATIC, docs/DECISION_LOG.md
// D-312): sub_140843730 parses the language wad's MSGS_TXT ("*<id>*\n<text>") into 16-byte records {char* text; int32 id;
// u16 lines} at *0x142ccbc38, count *0x142ccbc50, sorted by id; every prompt asks sub_140845140(id, &text), a bsearch
// over that table, when it is shown. The mod points the matching records' text at its own wording while gameplay first
// person with motion controls is on, and back at the game's own text otherwise. Any thread (called from the pad poll).
void PromptTextTick();
}  // namespace gowvr::fp
