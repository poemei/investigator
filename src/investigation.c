/* STN-LABZ Rictus Investigation module.
 * AI-assisted implementation: OpenAI Codex, 2026-08-27. Human review required. */
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openssl/evp.h>
#include "investigation.h"
#include "production.h"

#define RECORD_PATH "intelligence.records"
#define OUTPUT_DIR "state/investigation"
#define INDEX_PATH OUTPUT_DIR "/investigations.tsv"
#define CURSOR_PATH OUTPUT_DIR "/watch-cursors.tsv"
#define RELATION_PATH OUTPUT_DIR "/evidence-relationships.tsv"
#define NOTICE_PATH OUTPUT_DIR "/material-change.notices"
#define NOTICE_DELIVERED_PATH OUTPUT_DIR "/material-change.delivered"
#define LINE_MAXIMUM 65536
#define PATH_MAXIMUM 1024

typedef struct investigation_record {
    char id[32], source[256], title[1024], url[2048], published[256];
    char summary[8192], content[32768], fingerprint[128];
} investigation_record_t;

static int g_initialized;
static const rictus_module_host_t *g_host;
static pthread_t g_worker;
static int g_worker_active;
static pthread_mutex_t g_stop_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_stop_cond = PTHREAD_COND_INITIALIZER;
static int g_stop_requested;
typedef enum evidence_relationship { REL_SUPPORTING=1, REL_OPPOSING, REL_CONTRADICTORY, REL_DUPLICATE, REL_CONTEXTUAL, REL_NEGATIVE } evidence_relationship_t;
static const char *relationship_name(evidence_relationship_t r){switch(r){case REL_SUPPORTING:return "SUPPORTING";case REL_OPPOSING:return "OPPOSING";case REL_CONTRADICTORY:return "CONTRADICTORY";case REL_DUPLICATE:return "DUPLICATE";case REL_CONTEXTUAL:return "CONTEXTUAL";case REL_NEGATIVE:return "NEGATIVE";default:return "UNRESOLVED";}}

static int copy_text(char *destination, size_t size, const char *source)
{
    size_t length;
    if (destination == NULL || source == NULL || size == 0U) return 0;
    length = strlen(source);
    if (length >= size) { destination[0] = '\0'; return 0; }
    memcpy(destination, source, length + 1U);
    return 1;
}

static int valid_string(const char *s) { return s != NULL && s[0] != '\0'; }

static int valid_id(const char *s, const char *prefix)
{
    size_t i, n;
    if (!valid_string(s) || prefix == NULL) return 0;
    n = strlen(prefix);
    if (strncasecmp(s, prefix, n) != 0 || s[n] == '\0') return 0;
    for (i = n; s[i]; ++i)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

static void unescape(const char *input, char *output, size_t size)
{
    size_t i = 0, o = 0;
    if (output == NULL || size == 0) return;
    output[0] = '\0';
    if (input == NULL) return;
    while (input[i] && o + 1 < size) {
        if (input[i] == '\\' && input[i + 1]) {
            ++i;
            if (input[i] == 't') output[o++] = '\t';
            else if (input[i] == 'r') output[o++] = '\r';
            else if (input[i] == 'n') output[o++] = '\n';
            else output[o++] = input[i];
            ++i;
        } else output[o++] = input[i++];
    }
    output[o] = '\0';
}

static int load_record(const char *id, investigation_record_t *record)
{
    FILE *file = NULL;
    char *line;
    int found = 0;
    if (!valid_id(id, "INT-") || record == NULL) return 0;
    line = (char *)malloc(LINE_MAXIMUM);
    if (line == NULL) return 0;
    if (((file = fopen(RECORD_PATH, "r")) == NULL) || file == NULL) { free(line); return 0; }
    while (fgets(line, LINE_MAXIMUM, file)) {
        char *fields[8], *context = NULL, *token;
        size_t count = 0;
        line[strcspn(line, "\r\n")] = '\0';
        token = strtok_r(line, "\t", &context);
        while (token && count < 8) { fields[count++] = token; token = strtok_r(NULL, "\t", &context); }
        if ((count == 7 || count == 8) && strcasecmp(fields[0], id) == 0) {
            memset(record, 0, sizeof(*record));
            copy_text(record->id, sizeof(record->id), fields[0]);
            unescape(fields[1], record->source, sizeof(record->source));
            unescape(fields[2], record->title, sizeof(record->title));
            unescape(fields[3], record->url, sizeof(record->url));
            unescape(fields[4], record->published, sizeof(record->published));
            unescape(fields[5], record->summary, sizeof(record->summary));
            if (count == 8) { unescape(fields[6], record->content, sizeof(record->content)); unescape(fields[7], record->fingerprint, sizeof(record->fingerprint)); }
            else unescape(fields[6], record->fingerprint, sizeof(record->fingerprint));
            found = 1; break;
        }
    }
    fclose(file); free(line); return found;
}

static unsigned long id_hash(const char *s)
{
    unsigned long hash = 2166136261UL;
    while (s && *s) { hash ^= (unsigned char)*s++; hash *= 16777619UL; }
    return hash;
}

static void candidate_id_for(const char *intelligence_id, char output[32])
{ snprintf(output, 32, "CAN-%08lX", id_hash(intelligence_id)); }

static void timestamp_now(char output[32])
{
    time_t current = time(NULL);
    struct tm utc;
    if (current != (time_t)-1 && gmtime_r(&current, &utc) != NULL)
        strftime(output, 32, "%Y-%m-%dT%H:%M:%SZ", &utc);
    else
        output[0] = '\0';
}

static int ensure_directory(void)
{
    struct stat st;
    if (mkdir("state", 0750) != 0 && errno != EEXIST) return 0;
    if (mkdir(OUTPUT_DIR, 0750) != 0 && errno != EEXIST) return 0;
    return stat(OUTPUT_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

static int paths_for(const char *candidate, char artifact[PATH_MAXIMUM], char history[PATH_MAXIMUM])
{
    if (!valid_id(candidate, "CAN-")) return 0;
    snprintf(artifact, PATH_MAXIMUM, "%s/%s.candidate.md", OUTPUT_DIR, candidate);
    snprintf(history, PATH_MAXIMUM, "%s/%s.sha256.history", OUTPUT_DIR, candidate);
    return 1;
}

static int sha256_file(const char *path, char output[65])
{
    EVP_MD_CTX *context = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE], buffer[8192];
    unsigned int digest_length = 0, i;
    FILE *file = NULL;
    size_t count;
    int ok = 0;

    context = EVP_MD_CTX_new();
    if (context == NULL) goto done;
    if (EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) goto done;
    if (((file = fopen(path, "rb")) == NULL) || file == NULL) goto done;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        if (EVP_DigestUpdate(context, buffer, count) != 1) goto done;
    if (ferror(file)) goto done;
    if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1 || digest_length != 32) goto done;
    for (i = 0; i < digest_length; ++i) snprintf(output + i * 2, 3, "%02x", digest[i]);
    output[64] = '\0';
    ok = 1;

done:
    if (file != NULL) fclose(file);
    EVP_MD_CTX_free(context);
    return ok;
}

static int retain_hash(const char *candidate, const char *event, const char *timestamp,
    const char *artifact, char output[65])
{
    char ignored[PATH_MAXIMUM], history_path[PATH_MAXIMUM]; FILE *history = NULL;
    if (!sha256_file(artifact, output) || !paths_for(candidate, ignored, history_path)) return 0;
    if (((history = fopen(history_path, "a")) == NULL) || !history) return 0;
    if (fprintf(history, "%s\t%s\t%s\n", timestamp, event, output) < 0 || fflush(history) != 0) { fclose(history); return 0; }
    fclose(history); return 1;
}

static int index_find(const char *key, char candidate[32], int *active)
{
    FILE *file = NULL; char line[256]; int found = 0;
    if (((file = fopen(INDEX_PATH, "r")) == NULL) || !file) return 0;
    while (fgets(line, sizeof(line), file)) {
        char intel[32], can[32], state[16];
        if (sscanf(line, "%31s\t%31s\t%15s", intel, can, state) == 3 &&
            (strcasecmp(key, intel) == 0 || strcasecmp(key, can) == 0)) {
            copy_text(candidate, 32, can); if (active) *active = strcasecmp(state, "ACTIVE") == 0; found = 1; break;
        }
    }
    fclose(file); return found;
}

static int index_append(const char *intel, const char *candidate)
{
    FILE *file = NULL;
    if (((file = fopen(INDEX_PATH, "a")) == NULL) || !file) return 0;
    if (fprintf(file, "%s\t%s\tINACTIVE\n", intel, candidate) < 0 || fflush(file) != 0) { fclose(file); return 0; }
    fclose(file); return 1;
}

static int index_activate(const char *candidate)
{
    FILE *in = NULL, *out = NULL; char temp[PATH_MAXIMUM], line[256]; int found = 0;
    snprintf(temp, sizeof(temp), "%s.tmp", INDEX_PATH);
    if (((in = fopen(INDEX_PATH, "r")) == NULL) || !in) return 0;
    if (((out = fopen(temp, "w")) == NULL) || !out) { fclose(in); return 0; }
    while (fgets(line, sizeof(line), in)) {
        char intel[32], can[32], state[16];
        if (sscanf(line, "%31s\t%31s\t%15s", intel, can, state) == 3) {
            if (strcasecmp(can, candidate) == 0) { copy_text(state, sizeof(state), "ACTIVE"); found = 1; }
            fprintf(out, "%s\t%s\t%s\n", intel, can, state);
        }
    }
    fclose(in); if (fflush(out) != 0) found = 0; fclose(out);
    if (!found || rename(temp, INDEX_PATH) != 0) { unlink(temp); return 0; }
    return 1;
}

int rictus_investigation_init(void)
{ if (g_initialized) return RICTUS_INVESTIGATION_OK; if (!ensure_directory()) return RICTUS_INVESTIGATION_IO_ERROR; g_initialized = 1; return RICTUS_INVESTIGATION_OK; }

int rictus_investigation_candidate_create(const char *intel_id)
{
    investigation_record_t *r; char candidate[32], artifact[PATH_MAXIMUM], history[PATH_MAXIMUM];
    char timestamp[32], evidence[64], hash[65], existing[32]; FILE *file = NULL; int result = RICTUS_INVESTIGATION_IO_ERROR;
    if (!g_initialized) return RICTUS_INVESTIGATION_NOT_ACTIVE;
    if (!valid_id(intel_id, "INT-")) return RICTUS_INVESTIGATION_INVALID_ARGUMENT;
    if (index_find(intel_id, existing, NULL)) return RICTUS_INVESTIGATION_ALREADY_EXISTS;
    r = (investigation_record_t *)calloc(1, sizeof(*r)); if (!r) return RICTUS_INVESTIGATION_ERROR;
    if (!load_record(intel_id, r)) { free(r); return RICTUS_INVESTIGATION_NOT_FOUND; }
    candidate_id_for(intel_id, candidate); timestamp_now(timestamp); snprintf(evidence, sizeof(evidence), "SE-%s-0001", candidate + 4);
    paths_for(candidate, artifact, history);
    if (access(artifact, F_OK) == 0) { result = RICTUS_INVESTIGATION_ALREADY_EXISTS; goto done; }
    if (((file = fopen(artifact, "wb")) == NULL) || !file) goto done;
    if (fprintf(file,
        "# Investigation Candidate %s\n\n- Status: CANDIDATE\n- Watch: INACTIVE\n- Originating intelligence: %s\n"
        "- Created: %s\n- Current assessment: UNRESOLVED\n- SRT disposition: NOT_REQUESTED\n\n"
        "## Candidate Premise\n\nInvestigate the security relevance and evidentiary implications of the originating intelligence record.\n\n"
        "## Analytic Model\n\n### Primary Hypothesis\n\nThe originating condition materially intersects an STN-LABZ protected or deployed boundary.\n\n"
        "### Alternatives\n\n1. The activity is commodity noise with no applicable boundary.\n2. The reporting is duplicated or syndicated rather than independent corroboration.\n3. The condition is real but not applicable to the deployed environment.\n\n"
        "### Assumptions\n\n- Applicability, exposure, exploitability, and actor intent are not established.\n\n"
        "### Change Indicators\n\n- Independent authoritative corroboration.\n- Repeated protected-boundary observations.\n- Evidence of bypass, access, execution, exposure, persistence, or state change.\n\n"
        "## Originating Intelligence Record\n\n- Source: %s\n- Published: %s\n- Title: %s\n- URL: %s\n- Fingerprint: %s\n\n"
        "### Summary\n\n%s\n\n### Source Content\n\n%s\n\n## Evidence Chain\n\n### %s\n\n"
        "- Date/time: %s\n- Evidence source: %s\n- Relationship: UNRESOLVED / ORIGINATING EVIDENCE\n"
        "- Provenance: %s | %s\n\n%s\n\n## Evidence Gaps and Unresolved Questions\n\n"
        "- Independent corroboration has not yet been established.\n- Applicability and exposure remain unresolved.\n\n"
        "## Analytical Disposition\n\nInsufficient evidence for SRT recommendation. No lifecycle transition authorized.\n",
        candidate, r->id, timestamp, r->source, r->published, r->title, r->url, r->fingerprint,
        r->summary, r->content, evidence, timestamp, r->source, r->id, r->url, r->summary) < 0 || fflush(file) != 0) goto done;
    fclose(file); file = NULL;
    if (!retain_hash(candidate, evidence, timestamp, artifact, hash) || !index_append(r->id, candidate)) goto done;
    printf("[INVESTIGATION] Candidate created id=%s source=%s sha256=%s\n", candidate, r->id, hash); result = RICTUS_INVESTIGATION_OK;
done:
    if (file) fclose(file);
    if (result != RICTUS_INVESTIGATION_OK) {
        unlink(artifact);
        unlink(history);
    }
    free(r);
    return result;
}

int rictus_investigation_watch_start(const char *candidate)
{
    char resolved[32], artifact[PATH_MAXIMUM], history[PATH_MAXIMUM], timestamp[32], event[64], hash[65]; FILE *file = NULL; int active = 0;
    if (!g_initialized) return RICTUS_INVESTIGATION_NOT_ACTIVE;
    if (!valid_id(candidate, "CAN-")) return RICTUS_INVESTIGATION_INVALID_ARGUMENT;
    if (!index_find(candidate, resolved, &active)) return RICTUS_INVESTIGATION_NOT_FOUND;
    if (active) return RICTUS_INVESTIGATION_ALREADY_EXISTS;
    paths_for(candidate, artifact, history); timestamp_now(timestamp); snprintf(event, sizeof(event), "SE-%s-WATCH", candidate + 4);
    if (((file = fopen(artifact, "ab")) == NULL) || !file) return RICTUS_INVESTIGATION_IO_ERROR;
    if (fprintf(file, "\n### %s\n\n- Date/time: %s\n- Evidence source: Operator directive\n- Relationship: WATCH ACTIVATION\n"
        "- Provenance: explicit !watch command\n\nTargeted continuing investigation authorized. Scope remains limited to the candidate premise.\n",
        event, timestamp) < 0 || fflush(file) != 0) { fclose(file); return RICTUS_INVESTIGATION_IO_ERROR; }
    fclose(file);
    if (!retain_hash(candidate, event, timestamp, artifact, hash) || !index_activate(candidate)) return RICTUS_INVESTIGATION_IO_ERROR;
    printf("[INVESTIGATION] Watch ACTIVE id=%s sha256=%s\n", candidate, hash); return RICTUS_INVESTIGATION_OK;
}

static int evidence_append_typed(const char *candidate,const char *source,const char *intel_id,const char *rule,evidence_relationship_t relationship,const char *evidence_text)
{
    char resolved[32], artifact[PATH_MAXIMUM], history[PATH_MAXIMUM], timestamp[32], event[64], hash[65]; FILE *file = NULL; int active = 0;
    if (!g_initialized) return RICTUS_INVESTIGATION_NOT_ACTIVE;
    if (!valid_id(candidate, "CAN-") || !valid_string(source) || !valid_string(evidence_text)) return RICTUS_INVESTIGATION_INVALID_ARGUMENT;
    if (!index_find(candidate, resolved, &active)) return RICTUS_INVESTIGATION_NOT_FOUND;
    if (!active) return RICTUS_INVESTIGATION_NOT_ACTIVE;
    paths_for(candidate, artifact, history); timestamp_now(timestamp); snprintf(event, sizeof(event), "SE-%s-%08lX", candidate + 4, id_hash(evidence_text));
    if (((file = fopen(artifact, "ab")) == NULL) || !file) return RICTUS_INVESTIGATION_IO_ERROR;
    if (fprintf(file, "\n### %s\n\n- Date/time: %s\n- Evidence source: %s\n- Relationship: %s\n"
        "- Correlation rule: %s\n- Provenance: %s\n\n%s\n", event,timestamp,source,relationship_name(relationship),rule?rule:"operator supplied",intel_id?intel_id:source,evidence_text)<0||fflush(file)!=0){fclose(file);return RICTUS_INVESTIGATION_IO_ERROR;}
    fclose(file); if(!retain_hash(candidate,event,timestamp,artifact,hash))return RICTUS_INVESTIGATION_INTEGRITY_ERROR;
    if(((file = fopen(RELATION_PATH, "a")) != NULL)&&file){fprintf(file,"v=1\t%s\t%s\t%s\t%s\t%s\t%s\n",candidate,intel_id?intel_id:"OPERATOR",relationship_name(relationship),rule?rule:"operator supplied",timestamp,hash);fclose(file);}
    return RICTUS_INVESTIGATION_OK;
}

int rictus_investigation_evidence_append(const char *candidate,const char *source,const char *evidence_text)
{return evidence_append_typed(candidate,source,NULL,"operator supplied",REL_CONTEXTUAL,evidence_text);}

static int contains_ci(const char *s,const char *n){size_t z;const char *p;if(!s||!n||!*n)return 0;z=strlen(n);for(p=s;*p;++p)if(strncasecmp(p,n,z)==0)return 1;return 0;}
static int extract_cve(const char *s,char out[32]){const char *p;size_t n=0;if(!s)return 0;for(p=s;*p;++p)if(strncasecmp(p,"CVE-",4)==0)break;if(!*p)return 0;while(p[n]&&n<31&&((p[n]>='A'&&p[n]<='Z')||(p[n]>='a'&&p[n]<='z')||(p[n]>='0'&&p[n]<='9')||p[n]=='-')){out[n]=p[n];++n;}out[n]='\0';return n>4;}
static int correlate(const investigation_record_t *origin,const investigation_record_t *item,char rule[96])
{
    char a[32],b[32];static const char *products[]={"drupal","joomla","wordpress","php","nginx","apache","mariadb"};static const char *classes[]={"cross site scripting","xss","path traversal","access control","file upload","deserialization","sql injection","authentication"};size_t i,j;
    if(extract_cve(origin->title,a)||extract_cve(origin->summary,a)||extract_cve(origin->content,a))if((extract_cve(item->title,b)||extract_cve(item->summary,b)||extract_cve(item->content,b))&&strcasecmp(a,b)==0){snprintf(rule,96,"shared CVE %s",a);return 1;}
    for(i=0;i<2;++i){const char *path=i?"/app/core/config.php":"/app/core/mailer.php";if((contains_ci(origin->summary,path)||contains_ci(origin->content,path))&&(contains_ci(item->summary,path)||contains_ci(item->content,path))){snprintf(rule,96,"shared protected path %s",path);return 1;}}
    for(i=0;i<(sizeof(products) / sizeof(products[0]));++i)if((contains_ci(origin->title,products[i])||contains_ci(origin->summary,products[i]))&&(contains_ci(item->title,products[i])||contains_ci(item->summary,products[i])))for(j=0;j<(sizeof(classes) / sizeof(classes[0]));++j)if((contains_ci(origin->title,classes[j])||contains_ci(origin->summary,classes[j]))&&(contains_ci(item->title,classes[j])||contains_ci(item->summary,classes[j]))){snprintf(rule,96,"shared product %s and class %s",products[i],classes[j]);return 1;}
    return 0;
}

static unsigned long cursor_get(const char *candidate){FILE *f=NULL;char line[128],id[32];unsigned long value,last=0;if(((f = fopen(CURSOR_PATH, "r")) == NULL)||!f)return 0;while(fgets(line,sizeof(line),f))if(sscanf(line,"%31s\t%lu",id,&value)==2&&strcasecmp(id,candidate)==0)last=value;fclose(f);return last;}
static void cursor_set(const char *candidate,unsigned long value){FILE *f=NULL;if(((f = fopen(CURSOR_PATH, "a")) != NULL)&&f){fprintf(f,"%s\t%lu\n",candidate,value);fclose(f);}}
static int notice_delivered(const char *id){FILE *f=NULL;char line[64];if(((f = fopen(NOTICE_DELIVERED_PATH, "r")) == NULL)||!f)return 0;while(fgets(line,sizeof(line),f)){line[strcspn(line,"\r\n")]=0;if(strcasecmp(line,id)==0){fclose(f);return 1;}}fclose(f);return 0;}
static void notice_queue(const char *candidate,const char *intel,const char *rule){FILE *f=NULL;char id[32];snprintf(id,sizeof(id),"INVN-%08lX",id_hash(intel));if(((f = fopen(NOTICE_PATH, "a")) != NULL)&&f){fprintf(f,"%s\t%s\t%s\t%s\n",id,candidate,intel,rule);fclose(f);}}
static void notice_drain(void){FILE *f=NULL,*done=NULL;char line[256],id[32],candidate[32],intel[32],rule[128],message[390];if(!g_host||!g_host->send_message)return;if(((f = fopen(NOTICE_PATH, "r")) == NULL)||!f)return;while(fgets(line,sizeof(line),f))if(sscanf(line,"%31s\t%31s\t%31s\t%127[^\r\n]",id,candidate,intel,rule)==4&&!notice_delivered(id)){snprintf(message,sizeof(message),"PM STN_Boss :Investigation material change | %s | new supporting evidence %s | rule=%s | !inv show %s",candidate,intel,rule,candidate);if(!g_host->send_message(message))break;if(((done = fopen(NOTICE_DELIVERED_PATH, "a")) == NULL)||!done)break;fprintf(done,"%s\n",id);fclose(done);done=NULL;}fclose(f);}

static void scan_watch(const char *intel_id,const char *candidate)
{
    FILE *f=NULL;char *line;unsigned long row=0,cursor=cursor_get(candidate);investigation_record_t origin,item;if(!load_record(intel_id,&origin))return;line=(char*)malloc(LINE_MAXIMUM);if(!line)return;if(((f = fopen(RECORD_PATH, "r")) == NULL)||!f){free(line);return;}
    while(fgets(line,LINE_MAXIMUM,f)){char *fields[8],*ctx=NULL,*tok,rule[96],evidence[1200];size_t count=0;++row;if(row<=cursor)continue;line[strcspn(line,"\r\n")]='\0';tok=strtok_r(line,"\t",&ctx);while(tok&&count<8){fields[count++]=tok;tok=strtok_r(NULL,"\t",&ctx);}if(count<7||strcasecmp(fields[0],intel_id)==0)continue;memset(&item,0,sizeof(item));copy_text(item.id,sizeof(item.id),fields[0]);unescape(fields[1],item.source,sizeof(item.source));unescape(fields[2],item.title,sizeof(item.title));unescape(fields[5],item.summary,sizeof(item.summary));if(count==8)unescape(fields[6],item.content,sizeof(item.content));if(correlate(&origin,&item,rule)){snprintf(evidence,sizeof(evidence),"%s | %s | %s",item.id,item.title,item.summary);if(evidence_append_typed(candidate,item.source,item.id,rule,REL_SUPPORTING,evidence)==RICTUS_INVESTIGATION_OK)notice_queue(candidate,item.id,rule);}}
    fclose(f);if(row!=cursor)cursor_set(candidate,row);free(line);
}

static void scan_active_watches(void){FILE *f=NULL;char line[256];if(((f = fopen(INDEX_PATH, "r")) == NULL)||!f){notice_drain();return;}while(fgets(line,sizeof(line),f)){char intel[32],candidate[32],state[16];if(sscanf(line,"%31s\t%31s\t%15s",intel,candidate,state)==3&&strcasecmp(state,"ACTIVE")==0)scan_watch(intel,candidate);}fclose(f);notice_drain();}
static int investigation_stop_wait(unsigned long milliseconds)
{
    int stopped;
    struct timespec deadline;

    pthread_mutex_lock(&g_stop_mutex);
    if (!g_stop_requested && milliseconds > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(milliseconds / 1000UL);
        deadline.tv_nsec += (long)(milliseconds % 1000UL) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        while (!g_stop_requested) {
            int result = pthread_cond_timedwait(&g_stop_cond, &g_stop_mutex, &deadline);
            if (result == ETIMEDOUT || result != 0) break;
        }
    }
    stopped = g_stop_requested;
    pthread_mutex_unlock(&g_stop_mutex);
    return stopped;
}

static void *investigation_worker(void *unused)
{
    (void)unused;
    while (!investigation_stop_wait(10000UL)) scan_active_watches();
    return NULL;
}

int rictus_investigation_assess(const char *candidate)
{ char resolved[32]; int active = 0; if (!g_initialized) return RICTUS_INVESTIGATION_NOT_ACTIVE; if (!valid_id(candidate, "CAN-")) return RICTUS_INVESTIGATION_INVALID_ARGUMENT; if (!index_find(candidate, resolved, &active)) return RICTUS_INVESTIGATION_NOT_FOUND; return active ? RICTUS_INVESTIGATION_OK : RICTUS_INVESTIGATION_NOT_ACTIVE; }

void rictus_investigation_shutdown(void) { g_initialized = 0; }

static rictus_module_result_t command_candidate(const rictus_module_command_t *command, rictus_module_command_reply_fn reply, void *context, void *unused)
{
    int result; char candidate[32], response[256]; (void)unused;
    if (!command || !reply) return RICTUS_MODULE_ERR_INVALID_ARGUMENT;
    if (!command->arguments[0]) return reply(context, "Usage: !candidate INT-XXXXXXXX") ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_START_FAILED;
    result = rictus_investigation_candidate_create(command->arguments);
    if (result == RICTUS_INVESTIGATION_OK) { candidate_id_for(command->arguments, candidate); snprintf(response, sizeof(response), "CANDIDATE CREATED | %s | SOURCE %s", candidate, command->arguments); }
    else if (result == RICTUS_INVESTIGATION_ALREADY_EXISTS) { index_find(command->arguments, candidate, NULL); snprintf(response, sizeof(response), "CANDIDATE ALREADY EXISTS | %s | SOURCE %s", candidate, command->arguments); }
    else if (result == RICTUS_INVESTIGATION_NOT_FOUND) snprintf(response, sizeof(response), "CANDIDATE REFUSED | %s | INT NOT FOUND", command->arguments);
    else snprintf(response, sizeof(response), "CANDIDATE FAILED | %s | RESULT %d", command->arguments, result);
    return reply(context, response) ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_START_FAILED;
}

static rictus_module_result_t command_watch(const rictus_module_command_t *command, rictus_module_command_reply_fn reply, void *context, void *unused)
{
    int result; char response[256]; (void)unused;
    if (!command || !reply) return RICTUS_MODULE_ERR_INVALID_ARGUMENT;
    if (!command->arguments[0]) return reply(context, "Usage: !watch CAN-XXXXXXXX") ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_START_FAILED;
    result = rictus_investigation_watch_start(command->arguments);
    if (result == RICTUS_INVESTIGATION_OK) snprintf(response, sizeof(response), "WATCH ACTIVE | %s", command->arguments);
    else if (result == RICTUS_INVESTIGATION_ALREADY_EXISTS) snprintf(response, sizeof(response), "WATCH ALREADY ACTIVE | %s", command->arguments);
    else if (result == RICTUS_INVESTIGATION_NOT_FOUND) snprintf(response, sizeof(response), "WATCH REFUSED | %s | CANDIDATE NOT FOUND", command->arguments);
    else snprintf(response, sizeof(response), "WATCH FAILED | %s | RESULT %d", command->arguments, result);
    return reply(context, response) ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_START_FAILED;
}

static rictus_module_result_t command_inv(const rictus_module_command_t *command, rictus_module_command_reply_fn reply, void *context, void *unused)
{
    FILE *file = NULL; char line[256], response[256]; unsigned int candidates = 0, watches = 0; (void)unused;
    if (!command || !reply) return RICTUS_MODULE_ERR_INVALID_ARGUMENT;
    if(strncasecmp(command->arguments,"show ",5)==0){
        char candidate[32],id[32],rel[32],rule[96],stamp[32],hash[65];unsigned int supporting=0,opposing=0,contradictory=0,duplicate=0,contextual=0,negative=0;FILE *relations=NULL;
        copy_text(candidate,sizeof(candidate),command->arguments+5);if(!valid_id(candidate,"CAN-"))return reply(context,"Usage: !inv show CAN-XXXXXXXX")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;
        if(((relations = fopen(RELATION_PATH, "r")) != NULL)&&relations){while(fgets(line,sizeof(line),relations))if(sscanf(line,"v=1\t%31s\t%31s\t%31s\t%95[^\t]\t%31s\t%64s",id,response,rel,rule,stamp,hash)==6&&strcasecmp(id,candidate)==0){if(strcasecmp(rel,"SUPPORTING")==0)++supporting;else if(strcasecmp(rel,"OPPOSING")==0)++opposing;else if(strcasecmp(rel,"CONTRADICTORY")==0)++contradictory;else if(strcasecmp(rel,"DUPLICATE")==0)++duplicate;else if(strcasecmp(rel,"CONTEXTUAL")==0)++contextual;else if(strcasecmp(rel,"NEGATIVE")==0)++negative;}fclose(relations);}
        snprintf(response,sizeof(response),"%s | supporting=%u opposing=%u contradictory=%u negative=%u duplicate=%u contextual=%u",candidate,supporting,opposing,contradictory,negative,duplicate,contextual);if(!reply(context,response))return RICTUS_MODULE_ERR_START_FAILED;
        snprintf(response,sizeof(response),"Likelihood=%s | confidence=%s | human review=%s",supporting>=2&&contradictory==0?"LIKELY":"NOT ASSESSED",supporting>=2?"MODERATE":"LOW",contradictory||opposing?"RECOMMENDED":"NOT REQUIRED");if(!reply(context,response))return RICTUS_MODULE_ERR_START_FAILED;
        return reply(context,"No SRT transition, scope expansion, attribution, publication, or remediation is authorized.")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;
    }
    if(strcasecmp(command->arguments,"scan")==0){scan_active_watches();return reply(context,"Investigation scan complete | explainable relationships appended | lifecycle state unchanged")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    if (strcasecmp(command->arguments, "status") != 0)
        return reply(context, "Usage: !inv status|show CAN-XXXXXXXX|scan") ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_START_FAILED;
    if (((file = fopen(INDEX_PATH, "r")) != NULL) && file) {
        while (fgets(line, sizeof(line), file)) { ++candidates; if (strstr(line, "\tACTIVE") != NULL) ++watches; }
        fclose(file);
    }
    snprintf(response, sizeof(response), "Investigation %u.%u.%u | ABI %u.%u | state=%s",
        RICTUS_INVESTIGATION_VERSION_MAJOR, RICTUS_INVESTIGATION_VERSION_MINOR,
        RICTUS_INVESTIGATION_VERSION_PATCH, RICTUS_MODULE_API_MAJOR, RICTUS_MODULE_API_MINOR,
        g_initialized ? "ACTIVE" : "INACTIVE");
    if (!reply(context, response)) return RICTUS_MODULE_ERR_START_FAILED;
    snprintf(response, sizeof(response), "Candidates=%u | active watches=%u | correlation worker=%s | mode=SHADOW", candidates, watches,g_worker?"ACTIVE":"INACTIVE");
    return reply(context, response) ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_START_FAILED;
}

static rictus_module_result_t command_im(const rictus_module_command_t *command,rictus_module_command_reply_fn reply,void *context,void *unused)
{
    char id[32],response[700]={0},detail[500];const char *argument;char *space;rictus_im_metrics_t metrics;(void)unused;
    if(!command||!reply)return RICTUS_MODULE_ERR_INVALID_ARGUMENT;
    if(strcasecmp(command->arguments,"status")==0){rictus_im_metrics(&metrics);snprintf(response,sizeof(response),"IM production | drafts=%u review pass=%u review fail=%u approvals=%u feedback=%u",metrics.drafts,metrics.reviews_passed,metrics.reviews_failed,metrics.approvals,metrics.feedback_items);if(!reply(context,response))return RICTUS_MODULE_ERR_START_FAILED;return reply(context,"Publication=HUMAN CONTROLLED | automatic publication/SRT/Chain/RAG/remediation=PROHIBITED")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    if(strncasecmp(command->arguments,"draft ",6)==0){argument=command->arguments+6;if(rictus_im_draft(argument,command->sender,id,response,sizeof(response))){if(!reply(context,response))return RICTUS_MODULE_ERR_START_FAILED;snprintf(response,sizeof(response),"IM DRAFT CREATED | %s | !im review %s",id,id);return reply(context,response)?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}return reply(context,response)?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    if(strncasecmp(command->arguments,"show ",5)==0){if(!rictus_im_show(command->arguments+5,response,sizeof(response),detail,sizeof(detail)))return reply(context,"IM draft not found.")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;if(!reply(context,response))return RICTUS_MODULE_ERR_START_FAILED;return reply(context,detail)?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    if(strncasecmp(command->arguments,"review ",7)==0){if(!rictus_im_review(command->arguments+7,response,sizeof(response)))return reply(context,"IM review failed or draft not found.")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;return reply(context,response)?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    if(strncasecmp(command->arguments,"approve ",8)==0){if(strcasecmp(command->sender,"STN_Boss")!=0&&strcasecmp(command->account,"STN_Boss")!=0)return reply(context,"IM approval refused: direct STN_Boss role authority required.")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;if(!rictus_im_approve(command->arguments+8,command->sender,response,sizeof(response)))return reply(context,response[0]?response:"IM approval failed or draft not found.")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;return reply(context,response)?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    if(strncasecmp(command->arguments,"feedback ",9)==0){copy_text(detail,sizeof(detail),command->arguments+9);space=strchr(detail,' ');if(!space)return reply(context,"Usage: !im feedback IM-DRAFT-XXXXXXXX <feedback>")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;*space++='\0';if(!rictus_im_feedback(detail,command->sender,space,response,sizeof(response)))return reply(context,"IM feedback failed.")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;return reply(context,response)?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;}
    return reply(context,"Usage: !im status|draft CAN-*|show IM-DRAFT-*|review IM-DRAFT-*|approve IM-DRAFT-*|feedback IM-DRAFT-* <text>")?RICTUS_MODULE_OK:RICTUS_MODULE_ERR_START_FAILED;
}

static rictus_module_result_t qualify(rictus_module_qualification_result_t *result)
{
    unsigned int executed = 0, passed = 0, failed = 0; char candidate[32];
#define TEST(x) do { ++executed; if (x) ++passed; else ++failed; } while (0)
    if (!result) return RICTUS_MODULE_ERR_INVALID_ARGUMENT;
    TEST(RICTUS_MODULE_API_MAJOR == 1); TEST(RICTUS_MODULE_API_MINOR == 4); TEST(valid_id("INT-CB934528", "INT-"));
    TEST(!valid_id("CB934528", "INT-")); TEST(valid_id("CAN-12345678", "CAN-")); TEST(!valid_id("CAN-", "CAN-"));
    TEST(id_hash("INT-A") == id_hash("INT-A")); TEST(id_hash("INT-A") != id_hash("INT-B"));
    candidate_id_for("INT-A", candidate); TEST(valid_id(candidate, "CAN-")); TEST(!valid_string(NULL)); TEST(!valid_string("")); TEST(valid_string("evidence"));
    {investigation_record_t a,b;char rule[96];memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));copy_text(a.title,sizeof(a.title),"CVE-2026-1234 Drupal access control");copy_text(b.summary,sizeof(b.summary),"Independent CVE-2026-1234 report");TEST(correlate(&a,&b,rule)&&strstr(rule,"shared CVE")!=NULL);memset(&b,0,sizeof(b));copy_text(b.title,sizeof(b.title),"Unrelated satellite launch");TEST(!correlate(&a,&b,rule));}
    TEST(strcasecmp(relationship_name(REL_CONTRADICTORY),"CONTRADICTORY")==0);TEST(RICTUS_INVESTIGATION_VERSION_MINOR==3);
    {rictus_im_evidence_posture_t posture;TEST(!rictus_im_posture("INVALID",&posture));}
#undef TEST
    result->tests_executed = executed; result->tests_passed = passed; result->tests_failed = failed;
    result->negative_test_executed = 1; result->negative_test_passed = failed == 0;
    return executed >= RICTUS_MODULE_MIN_TESTS && failed == 0 ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_QUALIFICATION;
}

static rictus_module_result_t start(const rictus_module_host_t *host)
{
    if (!host || !host->register_command || !host->unregister_command) return RICTUS_MODULE_ERR_INVALID_ARGUMENT;
    if (rictus_investigation_init() != RICTUS_INVESTIGATION_OK) return RICTUS_MODULE_ERR_START_FAILED;
    g_host = host;
    if (!host->register_command("candidate", command_candidate, NULL)) goto fail;
    if (!host->register_command("watch", command_watch, NULL)) { host->unregister_command("candidate", NULL); goto fail; }
    if (!host->register_command("inv", command_inv, NULL)) { host->unregister_command("watch", NULL); host->unregister_command("candidate", NULL); goto fail; }
    if(!host->register_command("im",command_im,NULL)){host->unregister_command("inv",NULL);host->unregister_command("watch",NULL);host->unregister_command("candidate",NULL);goto fail;}
    pthread_mutex_lock(&g_stop_mutex);
    g_stop_requested = 0;
    pthread_mutex_unlock(&g_stop_mutex);
    if (pthread_create(&g_worker, NULL, investigation_worker, NULL) != 0) goto fail_commands;
    g_worker_active = 1;
    printf("[INVESTIGATION] Commands registered: candidate, watch, inv\n"); return RICTUS_MODULE_OK;
fail_commands: host->unregister_command("im",NULL);host->unregister_command("inv",NULL);host->unregister_command("watch",NULL);host->unregister_command("candidate",NULL);
fail: g_host = NULL; rictus_investigation_shutdown(); return RICTUS_MODULE_ERR_START_FAILED;
}

static rictus_module_result_t stop(void)
{
    int ok = 1;
    pthread_mutex_lock(&g_stop_mutex);
    g_stop_requested = 1;
    pthread_cond_broadcast(&g_stop_cond);
    pthread_mutex_unlock(&g_stop_mutex);
    if (g_worker_active) {
        if (pthread_join(g_worker, NULL) != 0) ok = 0;
        g_worker_active = 0;
    }
    if (g_host && g_host->unregister_command) { ok = g_host->unregister_command("im",NULL)&&ok; ok = g_host->unregister_command("inv", NULL) && ok; ok = g_host->unregister_command("watch", NULL) && ok; ok = g_host->unregister_command("candidate", NULL) && ok; }
    g_host = NULL; rictus_investigation_shutdown(); return ok ? RICTUS_MODULE_OK : RICTUS_MODULE_ERR_STOP_FAILED;
}

const rictus_module_descriptor_t rictus_investigation_descriptor = {
    RICTUS_INVESTIGATION_ID, RICTUS_INVESTIGATION_NAME, RICTUS_INVESTIGATION_VERSION_MAJOR,
    RICTUS_INVESTIGATION_VERSION_MINOR, RICTUS_INVESTIGATION_VERSION_PATCH,
    RICTUS_MODULE_API_MAJOR, RICTUS_MODULE_API_MINOR, qualify, start, stop
};

const rictus_module_descriptor_t *stnlabz_module_get_descriptor(void)
{ return &rictus_investigation_descriptor; }
