#ifndef TG_WORKFLOW_PLAN_H
#define TG_WORKFLOW_PLAN_H

/**
 * @file workflow_plan.h
 * @brief Startup compiler and immutable plans for business transactions.
 *
 * Configuration text and token indices belong to each compiled class plan.
 * Shard cloning creates independent documents; no token refers to the temporary
 * outer scenario parser or to a runtime response buffer.
 */

#include "../proto/proto.h"
#include "value.h"
/* Bounds cover visited-step bits, context slots, and request bytes plus NUL. */
#define TG_WF_STEPS 16U
#define TG_WF_VARS 32U
#define TG_WF_REQUEST_CAP 16385U
enum tg_step_kind { TG_STEP_HTTP, TG_STEP_DNS, TG_STEP_THINK, TG_STEP_BRANCH };

/**
 * @brief Prevalidated control flow and token locations for one step.
 *
 * object/extract/checks/condition refer to the owned JSON document. Missing
 * extract/checks use negative indices; condition is only valid for branches.
 * next/yes/no are forward step indices, with plan.count denoting transaction end.
 */
struct tg_step_plan {
        char name[64];
        enum tg_step_kind kind;
        int object, extract, checks, condition;
        unsigned next, yes, no, timeout_ms, min_ms, max_ms;
        bool capture_headers, capture_json; /**< Decoded response selector requirements. */
};

/** @brief Owned class document, compiled steps, and immutable dataset row indices. */
struct tg_workflow_plan {
        struct tg_document doc;
        struct tg_step_plan steps[TG_WF_STEPS];
        /* timeout_ms covers the whole business and does not reset between steps. */
        unsigned count, timeout_ms, row_count;
        /* vars is an optional object token; rows owns the dataset index array. */
        int vars, *rows;
};
extern const struct tg_proto_ops tg_workflow_proto_ops;

/**
 * @brief Copies and compiles a transaction subtree from the scenario document.
 *
 * File-backed datasets must already be expanded by the launcher.
 * @param out Receives the owned plan, released through config_free.
 * @return 0 on success; -1 without publishing a partial plan.
 */
int tg_workflow_compile(const struct tg_json_doc *doc, int token, void **out);

/**
 * @brief Finds a member token in the plan-owned document.
 *
 * @return A token index, or a negative value when the member is absent.
 */
int tg_wf_member(const struct tg_workflow_plan *p, int object, const char *key);

/**
 * @brief Decodes a string field, using fallback only when it is absent.
 *
 * @return 0 on success; -1 for a missing required value, wrong type, or overflow.
 */
int tg_wf_text(const struct tg_workflow_plan *p, int object, const char *key,
               const char *fallback, char *out, size_t cap);

/**
 * @brief Reads a bounded unsigned field or an already validated fallback.
 *
 * @return 0 on success; -1 for an invalid configured value.
 */
int tg_wf_u32(const struct tg_workflow_plan *p, int object, const char *key,
              unsigned fallback, unsigned min, unsigned max, unsigned *out);
#endif
