#ifndef RICTUS_INVESTIGATION_H
#define RICTUS_INVESTIGATION_H

#include "rictus_module.h"

#define RICTUS_INVESTIGATION_ID "investigation"
#define RICTUS_INVESTIGATION_NAME "Rictus Investigation"
#define RICTUS_INVESTIGATION_VERSION_MAJOR 0
#define RICTUS_INVESTIGATION_VERSION_MINOR 3
#define RICTUS_INVESTIGATION_VERSION_PATCH 0

/*
 * Rictus Investigation Module
 *
 * Provides evidence-driven investigation of operator-selected
 * intelligence candidates.
 *
 * MCR: 2026-08-27-IN
 *
 * This interface defines Investigation module-owned behavior.
 * STN-LABZ module lifecycle behavior remains defined by ABI 1.4.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Investigation module result codes.
 */
typedef enum rictus_investigation_result {
    RICTUS_INVESTIGATION_OK = 0,
    RICTUS_INVESTIGATION_ERROR = -1,
    RICTUS_INVESTIGATION_INVALID_ARGUMENT = -2,
    RICTUS_INVESTIGATION_NOT_FOUND = -3,
    RICTUS_INVESTIGATION_ALREADY_EXISTS = -4,
    RICTUS_INVESTIGATION_IO_ERROR = -5,
    RICTUS_INVESTIGATION_INTEGRITY_ERROR = -6,
    RICTUS_INVESTIGATION_NOT_ACTIVE = -7
} rictus_investigation_result_t;

/**
 * @brief Initialize Investigation module-owned state.
 *
 * This initializes Investigation capability state only.
 * Module lifecycle activation remains controlled through the
 * STN-LABZ ABI.
 *
 * @return RICTUS_INVESTIGATION_OK on success.
 */
int rictus_investigation_init(void);

/**
 * @brief Create an investigation candidate from an intelligence record.
 *
 * Implements the operator-directed:
 *
 *     !candidate INT-*
 *
 * handoff from Intelligence into Investigation.
 *
 * The operation creates the candidate Markdown artifact and initializes
 * its chronological evidence chain.
 *
 * @param intelligence_id Stable originating INT-* identifier.
 *
 * @return RICTUS_INVESTIGATION_OK on success or an Investigation
 *         result code on failure.
 */
int rictus_investigation_candidate_create(
    const char *intelligence_id
);

/**
 * @brief Place an existing candidate under active investigation.
 *
 * Implements:
 *
 *     !watch <candidate>
 *
 * The watch authorizes continuing targeted evidence collection and
 * analysis for the specified candidate.
 *
 * @param candidate_id Stable candidate identifier.
 *
 * @return RICTUS_INVESTIGATION_OK on success or an Investigation
 *         result code on failure.
 */
int rictus_investigation_watch_start(
    const char *candidate_id
);

/**
 * @brief Append material evidence to a candidate evidence chain.
 *
 * Evidence is appended to the existing candidate Markdown artifact.
 * Existing evidence-chain entries are preserved.
 *
 * A successful material update creates a new evidence state requiring
 * a new SHA-256 digest of the complete persisted candidate artifact.
 *
 * @param candidate_id Candidate receiving the evidence.
 * @param source Evidence source identifier or provenance reference.
 * @param evidence Evidence text to append.
 *
 * @return RICTUS_INVESTIGATION_OK on success or an Investigation
 *         result code on failure.
 */
int rictus_investigation_evidence_append(
    const char *candidate_id,
    const char *source,
    const char *evidence
);

/**
 * @brief Reassess the accumulated evidence for a candidate.
 *
 * Assessment may identify supporting, opposing, contradictory,
 * corroborating, or unresolved evidence and may produce an analytical
 * recommendation.
 *
 * Assessment does not perform an SRT lifecycle transition.
 *
 * @param candidate_id Candidate to reassess.
 *
 * @return RICTUS_INVESTIGATION_OK on success or an Investigation
 *         result code on failure.
 */
int rictus_investigation_assess(
    const char *candidate_id
);

/**
 * @brief Stop Investigation module-owned activity.
 *
 * Active investigation state is closed cleanly without altering
 * preserved candidate evidence artifacts.
 */
void rictus_investigation_shutdown(void);

extern const rictus_module_descriptor_t rictus_investigation_descriptor;

RICTUS_EXPORT
const rictus_module_descriptor_t *stnlabz_module_get_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif /* RICTUS_INVESTIGATION_H */
