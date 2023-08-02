#include "common.h"
#include "common-sdl.h"
#include "whisper.h"
#include "json.hpp"

#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <deque>

using json = nlohmann::json;

// command-line parameters
struct whisper_params {
    int32_t n_threads  = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t prompt_ms  = 5000;
    int32_t command_ms = 8000;
    int32_t capture_id = -1;
    int32_t max_tokens = 32;
    int32_t audio_ctx  = 0;

    float vad_thold    = 0.6f;
    float freq_thold   = 100.0f;

    bool speed_up      = false;
    bool translate     = false;
    bool print_special = false;
    bool print_energy  = false;

    std::string language  = "en";
    std::string model     = "models/ggml-base.en.bin";
};
struct command {
    std::vector<whisper_token> tokens;
    std::string plaintext;
};
struct commandset {
    std::vector<struct command> commands;
    std::vector<whisper_token> prompt_tokens;
    // TODO: Store longest command?
    // Multi-token commands should have probabilities of subsequent logits
    // given that the prior logit is correct.
    // In this case, all commands must be iterated.
    // This however, is likely highly involved as different tokens
    // almost certainly have different spoken lengths
    // It would also have performance implications equivalent to a beam search
};

void whisper_print_usage(int argc, char ** argv, const whisper_params & params);

bool whisper_params_parse(int argc, char ** argv, whisper_params & params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            whisper_print_usage(argc, argv, params);
            exit(0);
        }
        else if (arg == "-t"   || arg == "--threads")       { params.n_threads     = std::stoi(argv[++i]); }
        else if (arg == "-pms" || arg == "--prompt-ms")     { params.prompt_ms     = std::stoi(argv[++i]); }
        else if (arg == "-cms" || arg == "--command-ms")    { params.command_ms    = std::stoi(argv[++i]); }
        else if (arg == "-c"   || arg == "--capture")       { params.capture_id    = std::stoi(argv[++i]); }
        else if (arg == "-mt"  || arg == "--max-tokens")    { params.max_tokens    = std::stoi(argv[++i]); }
        else if (arg == "-ac"  || arg == "--audio-ctx")     { params.audio_ctx     = std::stoi(argv[++i]); }
        else if (arg == "-vth" || arg == "--vad-thold")     { params.vad_thold     = std::stof(argv[++i]); }
        else if (arg == "-fth" || arg == "--freq-thold")    { params.freq_thold    = std::stof(argv[++i]); }
        else if (arg == "-su"  || arg == "--speed-up")      { params.speed_up      = true; }
        else if (arg == "-tr"  || arg == "--translate")     { params.translate     = true; }
        else if (arg == "-ps"  || arg == "--print-special") { params.print_special = true; }
        else if (arg == "-pe"  || arg == "--print-energy")  { params.print_energy  = true; }
        else if (arg == "-l"   || arg == "--language")      { params.language      = argv[++i]; }
        else if (arg == "-m"   || arg == "--model")         { params.model         = argv[++i]; }
        else {
            fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            whisper_print_usage(argc, argv, params);
            exit(0);
        }
    }

    return true;
}

void whisper_print_usage(int /*argc*/, char ** argv, const whisper_params & params) {
    fprintf(stderr, "\n");
    fprintf(stderr, "usage: %s [options]\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -h,         --help           [default] show this help message and exit\n");
    fprintf(stderr, "  -t N,       --threads N      [%-7d] number of threads to use during computation\n", params.n_threads);
    fprintf(stderr, "  -pms N,     --prompt-ms N    [%-7d] prompt duration in milliseconds\n",             params.prompt_ms);
    fprintf(stderr, "  -cms N,     --command-ms N   [%-7d] command duration in milliseconds\n",            params.command_ms);
    fprintf(stderr, "  -c ID,      --capture ID     [%-7d] capture device ID\n",                           params.capture_id);
    fprintf(stderr, "  -mt N,      --max-tokens N   [%-7d] maximum number of tokens per audio chunk\n",    params.max_tokens);
    fprintf(stderr, "  -ac N,      --audio-ctx N    [%-7d] audio context size (0 - all)\n",                params.audio_ctx);
    fprintf(stderr, "  -vth N,     --vad-thold N    [%-7.2f] voice activity detection threshold\n",        params.vad_thold);
    fprintf(stderr, "  -fth N,     --freq-thold N   [%-7.2f] high-pass frequency cutoff\n",                params.freq_thold);
    fprintf(stderr, "  -su,        --speed-up       [%-7s] speed up audio by x2 (reduced accuracy)\n",     params.speed_up ? "true" : "false");
    fprintf(stderr, "  -tr,        --translate      [%-7s] translate from source language to english\n",   params.translate ? "true" : "false");
    fprintf(stderr, "  -ps,        --print-special  [%-7s] print special tokens\n",                        params.print_special ? "true" : "false");
    fprintf(stderr, "  -pe,        --print-energy   [%-7s] print sound energy (for debugging)\n",          params.print_energy ? "true" : "false");
    fprintf(stderr, "  -l LANG,    --language LANG  [%-7s] spoken language\n",                             params.language.c_str());
    fprintf(stderr, "  -m FNAME,   --model FNAME    [%-7s] model path\n",                                  params.model.c_str());
    fprintf(stderr, "\n");
}

json unguided_transcription(struct whisper_context * ctx, audio_async &audio, json jparams, const whisper_params &params) {
    using namespace std::chrono;
    // Length of a transcription chunk isn't important as long as timestamps are supported
    std::vector<whisper_token> prompt_tokens;
    milliseconds time_now = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch();
    milliseconds start_time = time_now;
    if (jparams.contains("timestamp")) {
        uint64_t ts = jparams.at("timestamp");
        start_time = milliseconds(ts);
    }
    if(time_now - start_time < milliseconds(1000)) {
        std::this_thread::sleep_for(milliseconds(1000) - (time_now - start_time));
    }

    // Wait until voice is detected
    time_now = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch();
    milliseconds window_duration = std::max(milliseconds(2000),time_now-start_time);
    std::vector<float> pcmf32_cur;
    audio.get(window_duration.count(), pcmf32_cur);
    while (!::vad_simple(pcmf32_cur, WHISPER_SAMPLE_RATE, 1000, params.vad_thold, params.freq_thold, params.print_energy)) {
        // TODO: find a way to poll commands/allow interrupts here
        // initial plan was to have incomplete commands yield back,
        // but passing state and calling a single poll here would be simpler...

        // If implemented, this wait could be reduced by the elapsed time,
        // but I expect polling to be quick unless a new message is sent
        // and the only likely message is a cancellation
        std::this_thread::sleep_for(milliseconds(100));
        time_now = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch();
        window_duration = std::max(milliseconds(2000),time_now-start_time);
        audio.get(window_duration.count(), pcmf32_cur);
    }
    // TODO: Improve windowing for unguided transcription.
    // If voice isn't detected, transcription should not occur
    // If voice isn't detected at the start, the timestamp should be ignored and
    //  a new starting timestamp should be made when it is.
    // If voice stops being detected, transcription should stop.
    // There should be a minimum length of time over which transcription occurs
    //  unless bounds are forced by voice detection
    // My current empirical understanding is that the VAD detects pauses in speech
    // For now, it'll be implemented to process either from start to now, or max samples
    // at VAD, but I'll probably pull the VAD method in and add detection for
    // rising edges in addition to falling edges to detect start of speech.
    audio.get((time_now-start_time).count(),pcmf32_cur);

    // TODO: swap to n_keep/param to make consistent with stream.cpp?
    uint64_t unprocessed_audio_timestamp = (time_now-milliseconds(200)).count();

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    if (jparams.contains("prompt")) {
        // unlikely to see much use. Under normal circumstances, no_context would be set to false
        std::string prompt = jparams.at("prompt");
        prompt_tokens.resize(1024);
        int n = whisper_tokenize(ctx, prompt.c_str(), prompt_tokens.data(), 1024);
        prompt_tokens.resize(n);

        wparams.prompt_tokens    = prompt_tokens.data();
        wparams.prompt_n_tokens  = prompt_tokens.size();
    }
    wparams.print_progress   = false;
    wparams.print_special    = params.print_special;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.translate        = params.translate;
    wparams.no_context       = jparams.value("no_context", true);
    wparams.single_segment   = true;
    wparams.max_tokens       = params.max_tokens;
    wparams.language         = params.language.c_str();
    wparams.n_threads        = params.n_threads;

    wparams.audio_ctx        = params.audio_ctx;
    wparams.speed_up         = params.speed_up;
    wparams.suppress_non_speech_tokens = true;
    // run the transformer and a single decoding pass
    if (whisper_full(ctx, wparams, pcmf32_cur.data(), pcmf32_cur.size()) != 0) {
        fprintf(stderr, "%s: ERROR: whisper_full() failed\n", __func__);
        throw json{
            {"code", -32803},
            {"message", "ERROR: whisper_full() failed"}
        };
    }
    std::string result = whisper_full_get_segment_text(ctx,0);
    return json {
        {"transcription", result},
        {"timestamp", unprocessed_audio_timestamp}
    };
}

// command-list mode
// guide the transcription to match the most likely command from a provided list
json guided_transcription(struct whisper_context * ctx, audio_async &audio, const whisper_params &params, json jparams, std::vector<struct commandset> commandset_list) {
    using namespace std::chrono;
    struct commandset cs = commandset_list[jparams.value("commandset_index", commandset_list.size()-1)];
    milliseconds time_now = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch();
    milliseconds start_time = time_now;
    if (jparams.contains("timestamp")) {
        uint64_t ts = jparams.at("timestamp");
        start_time = milliseconds(ts);
    }
    // Ensure minimum buffer
    if(time_now - start_time < milliseconds(1000)) {
        std::this_thread::sleep_for(milliseconds(1000) - (time_now - start_time));
    }

    // Wait until voice is detected
    time_now = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch();
    milliseconds window_duration = std::max(milliseconds(2000),time_now-start_time);
    std::vector<float> pcmf32_cur;
    audio.get(window_duration.count(), pcmf32_cur);
    while (!::vad_simple(pcmf32_cur, WHISPER_SAMPLE_RATE, 1000, params.vad_thold, params.freq_thold, params.print_energy)) {
        std::this_thread::sleep_for(milliseconds(100));
        time_now = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch();
        milliseconds window_duration = std::max(milliseconds(1000),time_now-start_time);
        audio.get(window_duration.count(), pcmf32_cur);
    }
    uint64_t unprocessed_audio_timestamp = time_now.count();

    fprintf(stderr, "%s: Speech detected! Processing ...\n", __func__);
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.print_progress   = false;
    wparams.print_special    = params.print_special;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.translate        = params.translate;
    wparams.no_context       = true;
    wparams.single_segment   = true;
    wparams.max_tokens       = 1;
    wparams.language         = params.language.c_str();
    wparams.n_threads        = params.n_threads;

    wparams.audio_ctx        = params.audio_ctx;
    wparams.speed_up         = params.speed_up;

    // TODO: Do some time testing. Does an overly long prompt slow down processing?
    // Set up command sets/precompute prompts
    wparams.prompt_tokens    = cs.prompt_tokens.data();
    wparams.prompt_n_tokens  = cs.prompt_tokens.size();
    // TODO: properly expose as option
    wparams.suppress_non_speech_tokens = true;

    // run the transformer and a single decoding pass
    if (whisper_full(ctx, wparams, pcmf32_cur.data(), pcmf32_cur.size()) != 0) {
        fprintf(stderr, "%s: ERROR: whisper_full() failed\n", __func__);
        throw json{
            {"code", -32803},
            {"message", "ERROR: whisper_full() failed"}//TODO: format string (sprintf?)
        };
    }

    // estimate command probability
    // NOTE: not optimal
    {
        const auto * logits = whisper_get_logits(ctx);

        std::vector<float> probs(whisper_n_vocab(ctx), 0.0f);

        // compute probs from logits via softmax
        {
            float max = -1e9;
            for (int i = 0; i < (int) probs.size(); ++i) {
                max = std::max(max, logits[i]);
            }

            float sum = 0.0f;
            for (int i = 0; i < (int) probs.size(); ++i) {
                probs[i] = expf(logits[i] - max);
                sum += probs[i];
            }

            for (int i = 0; i < (int) probs.size(); ++i) {
                probs[i] /= sum;
            }
        }

        std::vector<std::pair<float, int>> probs_id;

        // In my testing, the most verbose token is always the desired.
        // TODO: Trim commandset struct once efficacy has been verified
        for (int i = 0; i < (int) cs.commands.size(); ++i) {
            probs_id.emplace_back(probs[cs.commands[i].tokens[0]], i);
        }

        // sort descending
        {
            using pair_type = decltype(probs_id)::value_type;
            std::sort(probs_id.begin(), probs_id.end(), [](const pair_type & a, const pair_type & b) {
                    return a.first > b.first;
                    });
        }
        int id = probs_id[0].second;
        return json{
            {"command_index", id},
                {"command_text", cs.commands[id].plaintext},
                {"timestamp", unprocessed_audio_timestamp},
        };
    }
}

json register_commandset(struct whisper_context * ctx, json jparams, std::vector<struct commandset> &commandset_list) {
    // TODO: check for token collision
    struct commandset cs;

    std::string  k_prompt = " select one from the available words: ";
    whisper_token tokens[32];
    for (std::string s : jparams) {
        std::vector<whisper_token> token_vec;
        // The existing command implementation uses a nested for loop to tokenize single characters
        // I fail to see the purpose of this when ' a' has a wholly different pronunciation than the start of ' apple'
        const int n = whisper_tokenize(ctx, (" " + s).c_str(), tokens, 32);
        if (n < 0) {
            fprintf(stderr, "%s: error: failed to tokenize command '%s'\n", __func__, s.c_str());
            return 3;
        }
        token_vec.push_back(tokens[0]);
        if (n > 1) {// empty string if n=0? Should never occur
            fprintf(stderr, "%s: error: command is more than a single token: %s\n", __func__, s.c_str());
        }
        struct command command = {token_vec, s};
        cs.commands.push_back(command);
        k_prompt += s;
    }
    k_prompt = k_prompt.substr(0,k_prompt.length()-2) + ". Selected word:";
    cs.prompt_tokens.resize(1024);
    int n = whisper_tokenize(ctx, k_prompt.c_str(), cs.prompt_tokens.data(), 1024);
    cs.prompt_tokens.resize(n);
    // prepare response
    int index = commandset_list.size();
    commandset_list.push_back(cs);
    return json{{"index",index}};
}
json seek(struct whisper_context * ctx, audio_async &audio, json params) {
    // whisper_state has the pertinent offsets, but there also seem to be a large
    // number of scratch buffers that would prevent rewinding context in a manner similar to llama
    // I'll give this a another pass once everything else is implemented,
    // but for now, it's unsupported
    throw json{
        {"code", -32601},
            {"message", "Seeking is not yet supported."}
    };
}
json parse_job(const json &body, struct whisper_context * ctx, audio_async &audio, const whisper_params &params, std::vector<struct commandset> &commandset_list) {
    // See: https://www.jsonrpc.org/specification
    json id = body.at("id");
    try {
        std::string version = body.at("jsonrpc");
        if (version != "2.0") {
            // unsupported version
            throw json{
                {"code", -3260},
                    {"message", "invalid jsonrpc version"}
            };
        }
        std::string method = body.at("method");
        json jparams = json{{"dummy", "dummy"}};
        if (body.contains("params"))
            jparams = body.at("params");
        json res;
        // TODO: be consistent about argument order
        fprintf(stderr, "Dispatching a job\n");
        if (method == "unguided")                { res = unguided_transcription(ctx, audio, jparams, params); }
        else if (method == "guided")             { res = guided_transcription(ctx, audio, params, jparams, commandset_list); }
        else if (method == "seek")               { res = seek(ctx, audio, jparams); }
        else if (method == "registerCommandset") { res = register_commandset(ctx, jparams, commandset_list); }
        else if (method == "echo")               { res = jparams; }


        return json{
            {"jsonrpc", "2.0"},
                {"result", res},
                {"id", id}
        };
    } catch(json ex) {
        return json {
            {"jsonrpc", "2.0"},
                {"error", ex},
                {"id", id}
        };
    }
}

void process_loop(struct whisper_context * ctx, audio_async &audio, const whisper_params &params) {
    std::deque<json> jobqueue;
    std::vector<struct commandset> commandset_list;
    while (true) {
        // For eventual cancellation support, shouldn't block if job exists
        if (std::cin.rdbuf()->in_avail() > 22 || jobqueue.size() == 0) {
            int content_length;
            if (scanf("Content-Length: %d", &content_length) != 1) {
                fprintf(stderr, "Could not read input: %d", std::cin.peek());
                return;
            }
            // scanf leaves the new lines intact
            std::cin.ignore(2);
            if (std::cin.peek() != 13) {
                // Content-Type. jsonrpc necessitates utf8.
                std::cin.ignore(200,10);
            }
            std::cin.ignore(2);
            // A message is being sent and blocking is acceptable
            std::string content(content_length,'\0');
            std::cin.read(&content[0], content_length);
            json job = json::parse(content);
            // TODO: Some messages(cancellation) should skip queue here
            if (job.is_array()) {
                // response must also be batched. Will implement later
                // for (subjob : job.begin())
                // TODO: At the very least respond with an unsupported error.
            } else {
                jobqueue.push_back(job);
            }
        }
        assert(jobqueue.size() > 0);
        json job = jobqueue.front();
        json resp = parse_job(job, ctx, audio, params, commandset_list);
        if (resp != "unfinished") {
            jobqueue.pop_front();
            // send response
            std::string data = resp.dump(-1, ' ', false, json::error_handler_t::replace);
            fprintf(stdout, "Content-Length: %d\r\n\r\n%s\n", data.length()+1, data.c_str());
            std::cout.flush();

        }
    }
}

int main(int argc, char ** argv) {
    whisper_params params;
    if (whisper_params_parse(argc, argv, params) == false) {
        return 1;
    }

    if (whisper_lang_id(params.language.c_str()) == -1) {
        fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
        whisper_print_usage(argc, argv, params);
        exit(0);
    }

    // whisper init
    struct whisper_context * ctx = whisper_init_from_file(params.model.c_str());
    // init audio

    audio_async audio(30*1000);
    if (!audio.init(params.capture_id, WHISPER_SAMPLE_RATE)) {
        fprintf(stderr, "%s: audio.init() failed!\n", __func__);
        return 1;
    }

    audio.resume();
    // TODO: Investigate why this is required. An extra second of startup latency is not great
    // wait for 1 second to avoid any buffered noise
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    audio.clear();
    // TODO: consider some sort of indicator to designate loading has finished?
    // Potentially better for the client to just start with a non-blocking message (register commands)
    process_loop(ctx, audio, params);

    audio.pause();
    whisper_print_timings(ctx);
    whisper_free(ctx);

    return 0;
}