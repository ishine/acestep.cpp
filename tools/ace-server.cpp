// ace-server.cpp - HTTP server for ACE-Step music generation
//
// Single binary, two endpoints (POST /lm, POST /synth), one port.
// Models loaded at boot. Serial by default: one global mutex so LM and
// synth never overlap on the GPU. Use --parallelize if your GPU has
// enough VRAM to run both pipelines concurrently.

#include "audio-io.h"
#include "pipeline-lm.h"
#include "pipeline-synth.h"
#include "request.h"

// suppress warnings in third-party headers
#ifdef __GNUC__
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "httplib.h"
#ifdef __GNUC__
#    pragma GCC diagnostic pop
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// server instance pointer for the signal handler
static httplib::Server * g_svr = nullptr;

static void on_signal(int) {
    if (g_svr) {
        g_svr->stop();
    }
}

// global queue: tracks total requests in the system (waiting + running).
// both endpoints share the same counter. when full -> 503.
static std::atomic<int> g_queue_n{ 0 };
static int              g_max_queue = 4;

static bool queue_acquire(void) {
    int cur = g_queue_n.fetch_add(1, std::memory_order_relaxed);
    if (cur >= g_max_queue) {
        g_queue_n.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

static void queue_release(void) {
    g_queue_n.fetch_sub(1, std::memory_order_relaxed);
}

// pipeline mutexes.
// serial mode (default): both endpoints lock mtx_lm so they never overlap.
// parallel mode (--parallelize): each endpoint locks its own mutex.
static std::mutex mtx_lm;
static std::mutex mtx_synth;
static bool       g_parallelize = false;

// loaded pipeline contexts (NULL = not loaded, endpoint returns 501)
static AceLm *    g_ctx_lm    = nullptr;
static AceSynth * g_ctx_synth = nullptr;

// limits
static int g_max_batch = 1;
static int g_mp3_kbps  = 128;

// helper: set a JSON error response
static void json_error(httplib::Response & res, int status, const char * msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    res.status = status;
    res.set_content(buf, "application/json");
}

// helper: set a 503 with queue info
static void json_busy(httplib::Response & res) {
    int  n = g_queue_n.load(std::memory_order_relaxed);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"server busy\",\"queue\":%d,\"max_queue\":%d}", n, g_max_queue);
    res.status = 503;
    res.set_header("Retry-After", "5");
    res.set_content(buf, "application/json");
}

// POST /lm
// accepts: AceRequest JSON
// returns: JSON array of enriched AceRequests (batch_size controls count)
static void handle_lm(const httplib::Request & req, httplib::Response & res) {
    if (!g_ctx_lm) {
        json_error(res, 501, "LM not loaded");
        return;
    }

    if (!queue_acquire()) {
        json_busy(res);
        return;
    }

    // parse the incoming JSON body
    AceRequest ace_req;
    if (!request_parse_json(&ace_req, req.body.c_str())) {
        queue_release();
        json_error(res, 400, "invalid JSON");
        return;
    }

    // clamp batch_size to [1, max_batch]
    int batch_size = ace_req.batch_size;
    if (batch_size < 1) {
        batch_size = 1;
    }
    if (batch_size > g_max_batch) {
        batch_size = g_max_batch;
    }

    // run the LM pipeline under lock
    std::vector<AceRequest> out(batch_size);
    int                     rc;
    {
        std::lock_guard<std::mutex> lock(mtx_lm);
        rc = ace_lm_generate(g_ctx_lm, &ace_req, batch_size, out.data(), NULL, NULL);
    }
    queue_release();

    if (rc != 0) {
        json_error(res, 500, "LM generation failed");
        return;
    }

    // serialize output as a JSON array
    std::string body = "[";
    for (int i = 0; i < batch_size; i++) {
        if (i > 0) {
            body += ",";
        }
        body += request_to_json(&out[i]);
    }
    body += "]";

    res.set_content(body, "application/json");
}

// POST /synth
// accepts: AceRequest JSON (one request = one MP3)
// returns: raw MP3 bytes (audio/mpeg) with metadata headers
static void handle_synth(const httplib::Request & req, httplib::Response & res) {
    if (!g_ctx_synth) {
        json_error(res, 501, "synth not loaded");
        return;
    }

    if (!queue_acquire()) {
        json_busy(res);
        return;
    }

    // parse the incoming JSON body
    AceRequest ace_req;
    if (!request_parse_json(&ace_req, req.body.c_str())) {
        queue_release();
        json_error(res, 400, "invalid JSON");
        return;
    }

    // pick the right mutex: in serial mode both endpoints share mtx_lm,
    // in parallel mode synth gets its own mutex so LM can run alongside.
    std::mutex & mtx = g_parallelize ? mtx_synth : mtx_lm;

    // generate one track (batch_n=1 always, client sends N requests for N tracks)
    AceAudio audio = {};
    int      rc;
    auto     t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mtx);
        rc = ace_synth_generate(g_ctx_synth, &ace_req, NULL, 0,  // no source audio (v1: no cover/lego)
                                1, &audio);
    }
    auto t1 = std::chrono::steady_clock::now();

    if (rc != 0 || !audio.samples) {
        queue_release();
        json_error(res, 500, "synth generation failed");
        return;
    }

    float compute_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    float duration   = (float) audio.n_samples / 48000.0f;

    // peak normalize to 0 dBFS (audio_encode_mp3 does not normalize)
    int   n_total = audio.n_samples * 2;
    float peak    = 0.0f;
    for (int i = 0; i < n_total; i++) {
        float a = audio.samples[i] < 0.0f ? -audio.samples[i] : audio.samples[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak > 1e-8f && peak != 1.0f) {
        float gain = 1.0f / peak;
        for (int i = 0; i < n_total; i++) {
            audio.samples[i] *= gain;
        }
    }

    // encode to MP3 in memory
    std::string mp3 = audio_encode_mp3(audio.samples, audio.n_samples, 48000, g_mp3_kbps);
    ace_audio_free(&audio);
    queue_release();

    if (mp3.empty()) {
        json_error(res, 500, "MP3 encoding failed");
        return;
    }

    // metadata headers so the client knows what it got
    char val[64];
    snprintf(val, sizeof(val), "%lld", (long long) ace_req.seed);
    res.set_header("X-Seed", val);
    snprintf(val, sizeof(val), "%.2f", duration);
    res.set_header("X-Duration", val);
    snprintf(val, sizeof(val), "%.0f", compute_ms);
    res.set_header("X-Compute-Ms", val);

    res.set_content(mp3, "audio/mpeg");
}

// GET /health
static void handle_health(const httplib::Request &, httplib::Response & res) {
    int  q = g_queue_n.load(std::memory_order_relaxed);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\""
             ",\"lm_loaded\":%s"
             ",\"synth_loaded\":%s"
             ",\"queue\":%d"
             ",\"max_queue\":%d"
             ",\"parallelize\":%s}",
             g_ctx_lm ? "true" : "false", g_ctx_synth ? "true" : "false", q, g_max_queue,
             g_parallelize ? "true" : "false");
    res.set_content(buf, "application/json");
}

static void usage(const char * prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "LM model (optional, enables POST /lm):\n"
            "  --lm-model <gguf>         LM GGUF file\n"
            "  --lm-max-seq <N>          KV cache size (default: 8192)\n"
            "\n"
            "Synth models (enables POST /synth):\n"
            "  --text-encoder <gguf>     Text encoder GGUF file\n"
            "  --dit <gguf>              DiT GGUF file\n"
            "  --vae <gguf>              VAE GGUF file\n"
            "\n"
            "LoRA:\n"
            "  --lora <path>             LoRA safetensors file or directory\n"
            "  --lora-scale <float>      LoRA scaling factor (default: 1.0)\n"
            "\n"
            "VAE tiling (memory control):\n"
            "  --vae-chunk <N>           Latent frames per tile (default: 256)\n"
            "  --vae-overlap <N>         Overlap frames per side (default: 64)\n"
            "\n"
            "Output:\n"
            "  --mp3-bitrate <kbps>      MP3 bitrate (default: 128)\n"
            "\n"
            "Server:\n"
            "  --host <addr>             Listen address (default: 127.0.0.1)\n"
            "  --port <N>                Listen port (default: 8080)\n"
            "  --max-batch <N>           LM batch pre-alloc (default: 1)\n"
            "  --max-queue <N>           Global queue depth (default: 4)\n"
            "  --parallelize             Allow LM + synth concurrently\n"
            "\n"
            "Debug:\n"
            "  --no-fsm                  Disable FSM constrained decoding\n"
            "  --no-fa                   Disable flash attention\n",
            prog);
}

int main(int argc, char ** argv) {
    AceLmParams lm_params;
    ace_lm_default_params(&lm_params);

    AceSynthParams synth_params;
    ace_synth_default_params(&synth_params);

    const char * host = "127.0.0.1";
    int          port = 8080;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        // LM
        if (!strcmp(argv[i], "--lm-model") && i + 1 < argc) {
            lm_params.model_path = argv[++i];
        } else if (!strcmp(argv[i], "--lm-max-seq") && i + 1 < argc) {
            lm_params.max_seq = atoi(argv[++i]);

            // synth models
        } else if (!strcmp(argv[i], "--text-encoder") && i + 1 < argc) {
            synth_params.text_encoder_path = argv[++i];
        } else if (!strcmp(argv[i], "--dit") && i + 1 < argc) {
            synth_params.dit_path = argv[++i];
        } else if (!strcmp(argv[i], "--vae") && i + 1 < argc) {
            synth_params.vae_path = argv[++i];

            // lora
        } else if (!strcmp(argv[i], "--lora") && i + 1 < argc) {
            synth_params.lora_path = argv[++i];
        } else if (!strcmp(argv[i], "--lora-scale") && i + 1 < argc) {
            synth_params.lora_scale = (float) atof(argv[++i]);

            // vae tiling
        } else if (!strcmp(argv[i], "--vae-chunk") && i + 1 < argc) {
            synth_params.vae_chunk = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--vae-overlap") && i + 1 < argc) {
            synth_params.vae_overlap = atoi(argv[++i]);

            // output
        } else if (!strcmp(argv[i], "--mp3-bitrate") && i + 1 < argc) {
            g_mp3_kbps = atoi(argv[++i]);

            // server
        } else if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-batch") && i + 1 < argc) {
            g_max_batch = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-queue") && i + 1 < argc) {
            g_max_queue = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--parallelize")) {
            g_parallelize = true;

            // debug
        } else if (!strcmp(argv[i], "--no-fsm")) {
            lm_params.use_fsm = false;
        } else if (!strcmp(argv[i], "--no-fa")) {
            lm_params.use_fa    = false;
            synth_params.use_fa = false;

        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    // need at least one pipeline
    bool have_lm    = (lm_params.model_path != NULL);
    bool have_synth = (synth_params.text_encoder_path != NULL && synth_params.dit_path != NULL);

    if (!have_lm && !have_synth) {
        fprintf(stderr,
                "[Server] ERROR: provide --lm-model and/or "
                "(--text-encoder + --dit)\n");
        usage(argv[0]);
        return 1;
    }

    // clamp max_batch
    if (g_max_batch < 1) {
        g_max_batch = 1;
    }
    if (g_max_batch > 9) {
        g_max_batch = 9;
    }

    // load LM pipeline (optional)
    if (have_lm) {
        lm_params.max_batch = g_max_batch;
        fprintf(stderr, "[Server] Loading LM (max_batch=%d, max_seq=%d)...\n", g_max_batch, lm_params.max_seq);
        g_ctx_lm = ace_lm_load(&lm_params);
        if (!g_ctx_lm) {
            fprintf(stderr, "[Server] FATAL: LM load failed\n");
            return 1;
        }
    }

    // load synth pipeline (optional)
    if (have_synth) {
        fprintf(stderr, "[Server] Loading synth...\n");
        g_ctx_synth = ace_synth_load(&synth_params);
        if (!g_ctx_synth) {
            fprintf(stderr, "[Server] FATAL: synth load failed\n");
            if (g_ctx_lm) {
                ace_lm_free(g_ctx_lm);
            }
            return 1;
        }
    }

    // setup HTTP server
    httplib::Server svr;
    g_svr = &svr;

    // reject oversized bodies (largest real request is ~10KB)
    svr.set_payload_max_length(100 * 1024);

    // thread pool sized for queue depth + margin for /health
    int n_threads      = g_max_queue + 2;
    svr.new_task_queue = [n_threads]() {
        return new httplib::ThreadPool((size_t) n_threads);
    };

    svr.Post("/lm", handle_lm);
    svr.Post("/synth", handle_synth);
    svr.Get("/health", handle_health);

    // graceful shutdown on SIGINT/SIGTERM
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "[Server] Listening on %s:%d\n", host, port);
    fprintf(stderr, "[Server] LM=%s synth=%s parallelize=%s\n", g_ctx_lm ? "loaded" : "off",
            g_ctx_synth ? "loaded" : "off", g_parallelize ? "yes" : "no");
    fprintf(stderr, "[Server] max_batch=%d max_queue=%d mp3_kbps=%d\n", g_max_batch, g_max_queue, g_mp3_kbps);

    if (!svr.listen(host, port)) {
        fprintf(stderr, "[Server] FATAL: cannot bind %s:%d\n", host, port);
    }

    // cleanup
    fprintf(stderr, "\n[Server] Shutting down...\n");
    if (g_ctx_synth) {
        ace_synth_free(g_ctx_synth);
    }
    if (g_ctx_lm) {
        ace_lm_free(g_ctx_lm);
    }
    fprintf(stderr, "[Server] Done\n");
    return 0;
}
