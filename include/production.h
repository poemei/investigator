#ifndef RICTUS_INVESTIGATION_PRODUCTION_H
#define RICTUS_INVESTIGATION_PRODUCTION_H
#include <stddef.h>
#define RICTUS_IM_ID_MAX 32
#define RICTUS_IM_PATH_MAX 1024
typedef struct { unsigned int supporting,opposing,contradictory,negative,duplicate,contextual,sensors,independent_sources; } rictus_im_evidence_posture_t;
typedef struct { unsigned int drafts,reviews_passed,reviews_failed,approvals,feedback_items; } rictus_im_metrics_t;
int rictus_im_posture(const char *candidate,rictus_im_evidence_posture_t *posture);
int rictus_im_draft(const char *candidate,const char *operator_name,char id[RICTUS_IM_ID_MAX],char *reason,size_t reason_size);
int rictus_im_show(const char *id,char *summary,size_t summary_size,char *detail,size_t detail_size);
int rictus_im_review(const char *id,char *result,size_t result_size);
int rictus_im_approve(const char *id,const char *operator_name,char *result,size_t result_size);
int rictus_im_feedback(const char *id,const char *operator_name,const char *feedback,char *result,size_t result_size);
void rictus_im_metrics(rictus_im_metrics_t *metrics);
#endif
