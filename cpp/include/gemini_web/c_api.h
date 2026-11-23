#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef GWA_BUILD_DLL
#define GWA_API __declspec(dllexport)
#elif defined(GWA_USE_DLL)
#define GWA_API __declspec(dllimport)
#else
#define GWA_API
#endif
#else
#define GWA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gwa_client gwa_client_t;

typedef void (*gwa_partial_cb)(const char* snapshot, size_t len, void* userdata);

// Creates a client from a Cookie header (or cURL `-H 'cookie: ...'`).
// Returns NULL on invalid cookies and writes an error message into err_buf.
GWA_API gwa_client_t* gwa_client_create(const char* cookie_header, int account_slot,
                                        char* err_buf, size_t err_buf_len);

// Creates a client from GEMINI_COOKIE or ~/.gemini-web-to-api/session.json.
GWA_API gwa_client_t* gwa_client_create_from_session(const char* profile,
                                                     char* err_buf, size_t err_buf_len);

GWA_API void gwa_client_destroy(gwa_client_t* client);

// Sends a prompt (with optional image paths and streaming callback).
// Caller frees *out_answer and *out_thinking (if non-NULL) with gwa_free().
// Returns 1 on success, 0 on error (writing details to err_buf).
GWA_API int gwa_client_ask(gwa_client_t* client, const char* prompt, const char* model,
                           const char* effort, const char* const* image_paths,
                           size_t image_count, int continue_chat,
                           gwa_partial_cb on_partial, void* userdata,
                           char** out_answer, char** out_thinking,
                           char* err_buf, size_t err_buf_len);

// Fetches available models as a JSON array string. Caller frees *out_json with gwa_free().
GWA_API int gwa_client_list_models_json(gwa_client_t* client, char** out_json,
                                        char* err_buf, size_t err_buf_len);

// Fetches account quota usage as a JSON object string. Caller frees *out_json with gwa_free().
GWA_API int gwa_client_usage_json(gwa_client_t* client, char** out_json,
                                  char* err_buf, size_t err_buf_len);

// Rotates __Secure-<N>PSIDTS via accounts.google.com/RotateCookies.
GWA_API int gwa_client_rotate_cookies(gwa_client_t* client, int force,
                                      char* err_buf, size_t err_buf_len);

// Returns the current live Cookie header (including absorbed Set-Cookie rotations).
// Caller frees returned pointer with gwa_free().
GWA_API char* gwa_client_live_cookies(gwa_client_t* client);

// Resets the stored multi-turn conversation thread on this client.
GWA_API void gwa_client_reset_chat(gwa_client_t* client);

// Frees strings returned by gwa_client_* functions.
GWA_API void gwa_free(void* ptr);

#ifdef __cplusplus
}
#endif
