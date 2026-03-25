/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "common/recalld-config.h"
#include "common/recalld-storage.h"

/* Background AI processing pipeline.
 *
 * Runs a thread pool that picks up newly ingested captures and
 * processes them through external tools:
 *
 *   - Audio captures  → Whisper (speech-to-text)
 *   - Video/screenshot → Tesseract (OCR)
 *
 * Results are stored back into the captures table and FTS5 index
 * via storage_update_text_fields() so they become searchable. */

typedef struct AiProcessor AiProcessor;

int ai_processor_new(
                AiProcessor **ret,
                const QueryServiceConfig *config,
                StorageHandle *storage);

AiProcessor* ai_processor_free(AiProcessor *proc);

int ai_processor_start(AiProcessor *proc);
int ai_processor_stop(AiProcessor *proc);

bool ai_processor_is_running(const AiProcessor *proc);
