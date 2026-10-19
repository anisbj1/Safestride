#pragma once

// bw/core.h — umbrella header (F00 spec §6 D4).
// Includes every bw_core header so a consumer (and the composition root) compiles
// the WHOLE contract at once; contract_checks.h comes last and static_asserts every
// layout, so drift anywhere fails the consumer's build (D3).
//
// Prefer including the specific header you need (e.g. <bw/events.h>) in feature
// code — this umbrella exists for composition-root wiring and contract coverage.

#include <bw/alerts.h>
#include <bw/clock.h>
#include <bw/commands.h>
#include <bw/containers/latest_value.h>
#include <bw/containers/ring_buffer.h>
#include <bw/containers/spsc_queue.h>
#include <bw/context.h>
#include <bw/events.h>
#include <bw/fault.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/input.h>
#include <bw/mode_profile.h>
#include <bw/model.h>
#include <bw/platform_sync.h>
#include <bw/providers.h>
#include <bw/quality.h>
#include <bw/result.h>
#include <bw/state.h>
#include <bw/sync_transport.h>
#include <bw/task_health.h>
#include <bw/time.h>
#include <bw/types.h>

// Last: asserts over everything above.
#include <bw/contract_checks.h>
