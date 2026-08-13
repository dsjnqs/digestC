/*
 * Daily Digest generator — C version (trimmed).
 *
 * Builds a 6-section daily digest — Word, Fact, Quote, Image, Trivia,
 * and History "of the Day" — and writes it to README.md for GitHub
 * Pages. The 4 sections that weren't backed by a live API (Question,
 * Tip, Discovery, Idea) have been removed; every remaining section
 * pulls fresh content from a real external source each run.
 *
 * Dependencies: libcurl (HTTP) only. No JSON library — this file uses a
 * small, deliberately simple "find this key's string value" scanner
 * instead of a full JSON parser. That's enough for the flat, predictable
 * responses these APIs return, but it is NOT a general JSON parser:
 *   - it does not handle arbitrary nesting/arrays generically
 *   - it does not decode \uXXXX unicode escapes
 *   - it assumes the key of interest appears as "key":"value"
 * For the History section it also always takes the FIRST event of the
 * day rather than a random one, since picking a random array element
 * generically would need real array parsing. All of this is a deliberate
 * simplicity/dependency trade-off for a single portable .c file.
 *
 * Build:
 *   gcc -O2 -Wall -o fetch_daily fetch_daily.c -lcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

#define OUTPUT_FILE "README.md"

/* ------------------------------------------------------------------ */
/* HTTP fetch via libcurl                                              */
/* ------------------------------------------------------------------ */

struct MemBuffer {
    char *data;
    size_t size;
};

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    struct MemBuffer *mem = (struct MemBuffer *)userp;

    char *ptr = realloc(mem->data, mem->size + real_size + 1);
    if (!ptr) return 0; /* out of memory */

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = '\0';
    return real_size;
}

/* Returns a malloc'd, null-terminated response body, or NULL on failure.
 * On failure, *err_out (if non-NULL) is set to a static description. */
static char *http_get(const char *url, const char **err_out) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_out) *err_out = "curl_easy_init failed";
        return NULL;
    }

    struct MemBuffer mem = { .data = malloc(1), .size = 0 };
    mem.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&mem);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "DailyDigestBot/1.0 (GitHub Actions daily job, C version)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (err_out) *err_out = curl_easy_strerror(res);
        free(mem.data);
        return NULL;
    }
    if (http_code >= 400) {
        if (err_out) *err_out = "HTTP error response";
        free(mem.data);
        return NULL;
    }

    return mem.data; /* caller frees */
}

/* ------------------------------------------------------------------ */
/* Minimal JSON string-value scanner                                   */
/* ------------------------------------------------------------------ */

static size_t find_substr_offset(const char *hay, const char *needle, size_t start) {
    if (start > strlen(hay)) return (size_t)-1;
    const char *pos = strstr(hay + start, needle);
    if (!pos) return (size_t)-1;
    return (size_t)(pos - hay);
}

/* Finds "key":"value" starting the search at *cursor, extracts value
 * (respecting simple backslash-escaped quotes), unescapes \" \\ \/ \n \t,
 * advances *cursor to just past the value, and returns a malloc'd copy.
 * Returns NULL (cursor untouched) if not found. */
static char *json_extract(const char *json, const char *key, size_t *cursor) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    size_t at = find_substr_offset(json, pattern, *cursor);
    if (at == (size_t)-1) return NULL;

    size_t val_start = at + strlen(pattern);
    size_t i = val_start;
    size_t len = strlen(json);

    while (i < len) {
        if (json[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (json[i] == '"') break;
        i++;
    }
    if (i >= len) return NULL; /* unterminated */

    size_t raw_len = i - val_start;
    char *raw = malloc(raw_len + 1);
    memcpy(raw, json + val_start, raw_len);
    raw[raw_len] = '\0';

    /* Unescape common sequences in place (result is never longer). */
    char *out = malloc(raw_len + 1);
    size_t oi = 0;
    for (size_t ri = 0; ri < raw_len; ri++) {
        if (raw[ri] == '\\' && ri + 1 < raw_len) {
            char n = raw[ri + 1];
            if (n == 'n') { out[oi++] = '\n'; ri++; continue; }
            if (n == 't') { out[oi++] = '\t'; ri++; continue; }
            if (n == '"' || n == '\\' || n == '/') { out[oi++] = n; ri++; continue; }
        }
        out[oi++] = raw[ri];
    }
    out[oi] = '\0';
    free(raw);

    *cursor = i + 1;
    return out;
}

/* Extracts the first quoted string inside a top-level JSON array, e.g.
 * ["word"] -> "word". Used for the random-word API response. */
static char *json_array_first_string(const char *json) {
    size_t bracket = find_substr_offset(json, "[", 0);
    if (bracket == (size_t)-1) return NULL;
    size_t quote = find_substr_offset(json, "\"", bracket);
    if (quote == (size_t)-1) return NULL;

    size_t start = quote + 1;
    size_t len = strlen(json);
    size_t i = start;
    while (i < len && json[i] != '"') i++;
    if (i >= len) return NULL;

    size_t val_len = i - start;
    char *out = malloc(val_len + 1);
    memcpy(out, json + start, val_len);
    out[val_len] = '\0';
    return out;
}

/* Decodes a handful of common HTML entities in place (safe: every
 * replacement is <= the entity's length). Good enough for trivia text,
 * not a general HTML decoder. */
static void html_decode_inplace(char *s) {
    struct { const char *ent; char rep; } table[] = {
        {"&quot;", '"'}, {"&#039;", '\''}, {"&apos;", '\''},
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
    };
    char *read = s, *write = s;
    while (*read) {
        int matched = 0;
        for (size_t t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
            size_t elen = strlen(table[t].ent);
            if (strncmp(read, table[t].ent, elen) == 0) {
                *write++ = table[t].rep;
                read += elen;
                matched = 1;
                break;
            }
        }
        if (!matched) *write++ = *read++;
    }
    *write = '\0';
}

/* ------------------------------------------------------------------ */
/* Section builders — each appends to the output FILE*                 */
/* ------------------------------------------------------------------ */

static void section_header(FILE *f, const char *title, const char *description) {
    fprintf(f, "## %s\n_%s_\n\n", title, description);
}

static void section_footer(FILE *f, const char *source_name, const char *source_url) {
    if (source_url) {
        fprintf(f, "\n\n**Source:** [%s](%s)\n\n---\n\n", source_name, source_url);
    } else {
        fprintf(f, "\n\n**Source:** %s\n\n---\n\n", source_name);
    }
}

static void word_of_the_day(FILE *f) {
    section_header(f, "\xF0\x9F\x93\x96 Word of the Day",
                    "A vocabulary word, expression, or piece of language worth learning.");
    const char *err = NULL;
    char *word_json = http_get("https://random-word-api.herokuapp.com/word", &err);
    char *word = word_json ? json_array_first_string(word_json) : NULL;

    if (word) {
        char url[256];
        snprintf(url, sizeof(url), "https://api.dictionaryapi.dev/api/v2/entries/en/%s", word);
        char *def_json = http_get(url, &err);
        size_t cur = 0;
        char *pos = def_json ? json_extract(def_json, "partOfSpeech", &cur) : NULL;
        char *definition = def_json ? json_extract(def_json, "definition", &cur) : NULL;

        if (definition) {
            fprintf(f, "**%s** *(%s)* - %s", word, pos ? pos : "", definition);
        } else {
            fprintf(f, "**%s** - (definition lookup failed: %s)", word,
                     err ? err : "no definition found");
        }
        free(pos);
        free(definition);
        free(def_json);
    } else {
        fprintf(f, "**serendipity** *(noun)* - the occurrence of finding pleasant things by "
                    "chance\n\n_(Live lookup failed: %s - showing a fallback word.)_",
                 err ? err : "unknown error");
    }
    free(word);
    free(word_json);
    section_footer(f, "Random Word API + Free Dictionary API", "https://dictionaryapi.dev/");
}

static void fact_of_the_day(FILE *f) {
    section_header(f, "\xF0\x9F\x92\xA1 Fact of the Day", "Something true, interesting, or surprising.");
    const char *err = NULL;
    char *json = http_get("https://uselessfacts.jsph.pl/api/v2/facts/random?language=en", &err);
    size_t cur = 0;
    char *text = json ? json_extract(json, "text", &cur) : NULL;

    if (text) {
        fprintf(f, "%s", text);
    } else {
        fprintf(f, "Honey never spoils - archaeologists have found 3,000-year-old edible honey "
                    "in Egyptian tombs.\n\n_(Live lookup failed: %s - showing a fallback fact.)_",
                 err ? err : "unknown error");
    }
    free(text);
    free(json);
    section_footer(f, "Useless Facts API", "https://uselessfacts.jsph.pl/");
}

static void quote_of_the_day(FILE *f) {
    section_header(f, "\xF0\x9F\x92\xAC Quote of the Day", "A quote - inspirational, funny, or philosophical.");
    const char *err = NULL;
    char *json = http_get("https://zenquotes.io/api/today", &err);
    size_t cur = 0;
    char *q = json ? json_extract(json, "q", &cur) : NULL;
    char *a = json ? json_extract(json, "a", &cur) : NULL;

    if (q && a) {
        fprintf(f, "\"%s\" - %s", q, a);
    } else {
        fprintf(f, "\"The only way to do great work is to love what you do.\" - Steve Jobs"
                    "\n\n_(Live lookup failed: %s - showing a fallback quote.)_",
                 err ? err : "unknown error");
    }
    free(q);
    free(a);
    free(json);
    section_footer(f, "ZenQuotes API", "https://zenquotes.io/");
}

static void image_of_the_day(FILE *f, struct tm *utc) {
    section_header(f, "\xF0\x9F\x8C\x8C Image of the Day", "Wikipedia's Picture of the Day.");
    char url[256];
    snprintf(url, sizeof(url), "https://en.wikipedia.org/api/rest_v1/feed/featured/%04d/%02d/%02d",
             utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday);

    const char *err = NULL;
    char *json = http_get(url, &err);

    if (!json) {
        fprintf(f, "_Couldn't reach Wikipedia's Picture of the Day feed today._\n\n**Error:** `%s`",
                 err ? err : "unknown error");
        section_footer(f, "Wikipedia Picture of the Day (Wikimedia REST API)",
                        "https://en.wikipedia.org/wiki/Main_Page");
        return;
    }

    size_t obj_start = find_substr_offset(json, "\"image\":{", 0);
    if (obj_start == (size_t)-1) {
        fprintf(f, "_No Picture of the Day entry found for today yet._");
        section_footer(f, "Wikipedia Picture of the Day (Wikimedia REST API)",
                        "https://en.wikipedia.org/wiki/Main_Page");
        free(json);
        return;
    }

    size_t cur = obj_start;
    char *title = json_extract(json, "title", &cur);
    char *thumb_source = json_extract(json, "source", &cur);
    char *full_source = json_extract(json, "source", &cur);
    char *description = json_extract(json, "text", &cur);
    char *file_page = json_extract(json, "file_page", &cur);

    char *img_url = full_source ? full_source : thumb_source;
    char *clean_title = NULL;
    if (title) {
        /* Strip a leading "File:" prefix and swap underscores for spaces. */
        const char *t = title;
        if (strncmp(t, "File:", 5) == 0) t += 5;
        clean_title = strdup(t);
        for (char *p = clean_title; *p; p++) if (*p == '_') *p = ' ';
    }

    if (img_url && clean_title) {
        fprintf(f, "![%s](%s)\n\n**%s**\n\n%s", clean_title, img_url, clean_title,
                 description ? description : "");
        if (file_page) fprintf(f, "\n\n[View full details on Wikimedia Commons](%s)", file_page);
    } else {
        fprintf(f, "_Today's Picture of the Day entry was missing expected fields._");
    }

    free(title);
    free(clean_title);
    free(thumb_source);
    free(full_source);
    free(description);
    free(file_page);
    free(json);
    section_footer(f, "Wikipedia Picture of the Day (Wikimedia REST API)",
                    "https://en.wikipedia.org/wiki/Main_Page");
}

static void trivia_of_the_day(FILE *f) {
    section_header(f, "\xF0\x9F\xA7\xA0 Trivia of the Day", "A quick knowledge challenge.");
    const char *err = NULL;
    char *json = http_get("https://opentdb.com/api.php?amount=1&type=multiple", &err);
    size_t cur = 0;
    char *category = json ? json_extract(json, "category", &cur) : NULL;
    cur = 0;
    char *difficulty = json ? json_extract(json, "difficulty", &cur) : NULL;
    cur = 0;
    char *question = json ? json_extract(json, "question", &cur) : NULL;
    cur = 0;
    char *correct = json ? json_extract(json, "correct_answer", &cur) : NULL;

    if (question && correct) {
        html_decode_inplace(question);
        html_decode_inplace(correct);
        if (category) html_decode_inplace(category);
        fprintf(f, "**[%s%s%s]** %s\n> Answer: ||%s||",
                 category ? category : "", (category && difficulty) ? " - " : "",
                 difficulty ? difficulty : "", question, correct);
    } else {
        fprintf(f, "**[Geography - Easy]** What is the smallest country in the world?\n"
                    "> Answer: ||Vatican City||\n\n_(Live lookup failed: %s - showing a fallback question.)_",
                 err ? err : "unknown error");
    }
    free(category);
    free(difficulty);
    free(question);
    free(correct);
    free(json);
    section_footer(f, "Open Trivia Database", "https://opentdb.com/");
}

static void history_of_the_day(FILE *f, struct tm *utc) {
    section_header(f, "\xF0\x9F\x95\xB0\xEF\xB8\x8F History of the Day",
                    "An event, person, or moment from history on this date.");
    char url[256];
    snprintf(url, sizeof(url), "https://byabbe.se/on-this-day/%d/%d/events.json",
             utc->tm_mon + 1, utc->tm_mday);
    const char *err = NULL;
    char *json = http_get(url, &err);
    size_t cur = 0;
    char *year = json ? json_extract(json, "year", &cur) : NULL;
    cur = 0;
    char *description = json ? json_extract(json, "description", &cur) : NULL;

    if (year && description) {
        fprintf(f, "**%s** - %s", year, description);
    } else {
        fprintf(f, "On this day in history, countless events shaped the world."
                    "\n\n_(Live lookup failed: %s - check back tomorrow.)_",
                 err ? err : "unknown error");
    }
    free(year);
    free(description);
    free(json);
    section_footer(f, "On This Day API (byabbe.se)", "https://byabbe.se/on-this-day/");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    FILE *f = fopen(OUTPUT_FILE, "w");
    if (!f) {
        fprintf(stderr, "Could not open %s for writing\n", OUTPUT_FILE);
        curl_global_cleanup();
        return 1;
    }

    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);

    fprintf(f, "# Daily Digest - %s\n\n", date_str);
    fprintf(f, "_A daily digest, refreshed automatically once a day._\n\n");

    word_of_the_day(f);
    fact_of_the_day(f);
    quote_of_the_day(f);
    image_of_the_day(f, &utc);
    trivia_of_the_day(f);
    history_of_the_day(f, &utc);

    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);
    fprintf(f, "_Last updated: %s UTC_\n", ts);

    fclose(f);
    curl_global_cleanup();

    printf("Wrote digest to %s\n", OUTPUT_FILE);
    return 0;
}
