#pragma once

namespace papertrail::worker {

// Says sentences with Kokoro's natural voices, for the reader, so that whatever a document's text does to the voice
// costs only this process. The reader starts one while it reads aloud with a natural voice.
//
// Reads one JSON request per line on stdin, and answers each with a JSON line on stdout, followed by "bytes" bytes of
// 16-bit mono samples at "rate" for a sentence said:
//   {"op":"say","id":1,"folder":"...","speaker":21,"british":true,"speed":1.0,"text":"..."}
//   {"op":"version","id":2}
// The model is loaded when first needed, and again when the folder or accent changes.
//
// With PAPERTRAIL_VOICE_TEST_TONE set, which only tests do, it needs no model and says each sentence as a short tone.
// With PAPERTRAIL_WORKER_TEST_FAULTS set too, a sentence containing __crash__ or __hang__ does that.
int runVoiceHost();

} // namespace papertrail::worker
