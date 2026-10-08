#pragma once
// H2 collision viewer logging -> HCM's plog (printf-style, prefixed).
#include <cstdio>
#define LOGF(...) do { char h2cvLogBuf_[1024]; snprintf(h2cvLogBuf_, sizeof(h2cvLogBuf_), __VA_ARGS__); PLOG_INFO << "[CollisionViewer] " << h2cvLogBuf_; } while (0)
