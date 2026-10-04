#include "Parameters.h"
#include "Test.h"
#include <clap/clap.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using namespace granular;
static unsigned rescans = 0;
void CLAP_ABI rescan(const clap_host_t*, clap_param_rescan_flags flags) { CHECK(flags == CLAP_PARAM_RESCAN_VALUES); ++rescans; }
const clap_host_params_t hostParams {rescan, nullptr, nullptr};
const void* CLAP_ABI hostExtension(const clap_host_t*, const char* id) { return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &hostParams : nullptr; }
void CLAP_ABI hostNoop(const clap_host_t*) {}
const clap_host_t host {CLAP_VERSION, nullptr, "Tests", "Tests", "", "1", hostExtension, hostNoop, hostNoop, hostNoop};

struct Events {
    std::vector<clap_event_param_value_t> data;
    clap_input_events_t api {this,
        [](const clap_input_events_t* e) -> uint32_t { return static_cast<uint32_t>(static_cast<Events*>(e->ctx)->data.size()); },
        [](const clap_input_events_t* e, uint32_t i) -> const clap_event_header_t* { return &static_cast<Events*>(e->ctx)->data.at(i).header; }};
    void add(clap_id id, double value, uint32_t time = 0) {
        clap_event_param_value_t e {};
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = id; e.value = value;
        e.note_id = e.port_index = e.channel = e.key = -1;
        data.push_back(e);
    }
};
struct MemoryStream {
    std::vector<uint8_t> data;
    size_t position = 0;
    bool fail = false;
    clap_ostream_t output {this, [](const clap_ostream_t* s, const void* bytes, uint64_t count) -> int64_t {
        auto& m = *static_cast<MemoryStream*>(s->ctx);
        if (m.fail) return 0;
        count = std::min(count, uint64_t(3));
        const auto* begin = static_cast<const uint8_t*>(bytes);
        m.data.insert(m.data.end(), begin, begin+count);
        return static_cast<int64_t>(count);
    }};
    clap_istream_t input {this, [](const clap_istream_t* s, void* bytes, uint64_t count) -> int64_t {
        auto& m = *static_cast<MemoryStream*>(s->ctx);
        if (m.fail) return -1;
        count = std::min({count, uint64_t(5), static_cast<uint64_t>(m.data.size()-m.position)});
        std::memcpy(bytes, m.data.data()+m.position, count); m.position += count;
        return static_cast<int64_t>(count);
    }};
};

struct Fixture {
    const clap_plugin_t* plugin;
    const clap_plugin_params_t* params;
    const clap_plugin_state_t* state;
    std::array<float, 512> left {}, right {};
    float* channels[2] {left.data(), right.data()};
    clap_audio_buffer_t buffer {channels, nullptr, 2, 0, 0};
    Fixture() {
        const auto* f = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        CHECK(f && f->get_plugin_count(f) == 1);
        CHECK(f->get_plugin_descriptor(f, 1) == nullptr);
        CHECK(f->create_plugin(f, &host, "unknown") == nullptr);
        plugin = f->create_plugin(f, &host, f->get_plugin_descriptor(f, 0)->id);
        CHECK(plugin && plugin->init(plugin));
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        CHECK(params && state);
    }
    ~Fixture() { plugin->stop_processing(plugin); plugin->deactivate(plugin); plugin->destroy(plugin); }
    void set(clap_id id, double value) { Events e; e.add(id, value); params->flush(plugin, &e.api, nullptr); }
    double get(clap_id id) { double value = 0; CHECK(params->get_value(plugin, id, &value)); return value; }
    void activate(double rate = 48000) { CHECK(plugin->activate(plugin, rate, 1, 512)); CHECK(plugin->start_processing(plugin)); }
    void process(uint32_t frames, Events* events = nullptr) {
        clap_process_t p {};
        p.steady_time = -1; p.frames_count = frames;
        p.audio_inputs = &buffer; p.audio_outputs = &buffer;
        p.audio_inputs_count = p.audio_outputs_count = 1;
        p.in_events = events ? &events->api : nullptr;
        CHECK(plugin->process(plugin, &p) == CLAP_PROCESS_CONTINUE);
    }
};

void stateAndParameters() {
    Fixture f;
    CHECK(f.params->count(f.plugin) == ParamCount);
    for (uint32_t id = 0; id < ParamCount; ++id) {
        clap_param_info_t info {};
        CHECK(f.params->get_info(f.plugin, id, &info));
        CHECK(info.id == id && info.default_value == f.get(id) && info.default_value == parameters[id].initial);
        CHECK(bool(info.flags & CLAP_PARAM_IS_STEPPED) == parameters[id].stepped);
        CHECK(bool(info.flags & CLAP_PARAM_IS_MODULATABLE) == !parameters[id].stepped);
        for (double v : {info.min_value, info.default_value, info.max_value}) {
            char text[128] {}; double parsed = -1;
            CHECK(f.params->value_to_text(f.plugin, id, v, text, sizeof(text)));
            CHECK(f.params->text_to_value(f.plugin, id, text, &parsed));
            CHECK(std::abs(parsed-v) <= std::abs(v)*.001+.06); // displayed text is rounded
        }
    }
    CHECK(!f.params->get_info(f.plugin, ParamCount, nullptr));
    double value = 0;
    CHECK(!f.params->text_to_value(f.plugin, Mix, "NaN", &value));
    CHECK(!f.params->text_to_value(f.plugin, Mix, "50 garbage", &value));
    CHECK(f.params->text_to_value(f.plugin, Time, "1.5 s", &value) && value == 1500);
    CHECK(f.params->text_to_value(f.plugin, Tone, "4 kHz", &value) && value == 4000);
    CHECK(f.params->text_to_value(f.plugin, Voices, "Minor", &value) && value == 6);
    CHECK(f.params->text_to_value(f.plugin, WarpType, "Whisper", &value) && value == 3);
    CHECK(f.params->text_to_value(f.plugin, Character, "Spectral", &value) && value == 1);

    f.set(Interval, 137.5); f.set(Character, 1); f.set(Voices, 10); f.set(Warp, 62); f.set(Time, 777);
    f.set(WarpType, 4); f.set(Sync, 1); f.set(Division, 7); f.set(Freeze, 1);
    MemoryStream stream; CHECK(f.state->save(f.plugin, &stream.output));
    f.set(Interval, 10); f.set(Character, 0); f.set(Voices, 0); f.set(Freeze, 0); f.set(Warp, 0);
    CHECK(f.state->load(f.plugin, &stream.input));
    CHECK(f.get(Interval) == 137.5 && f.get(Character) == 1 && f.get(Voices) == 10 && f.get(Warp) == 62);
    CHECK(f.get(Time) == 777 && f.get(WarpType) == 4 && f.get(Sync) == 1 && f.get(Division) == 7 && f.get(Freeze) == 1);
    CHECK(rescans > 0);
    // Truncated, failing, and corrupted streams leave the current state alone.
    stream.data.pop_back(); stream.position = 0;
    f.set(Interval, 33);
    CHECK(!f.state->load(f.plugin, &stream.input)); CHECK(f.get(Interval) == 33);
    stream.fail = true; CHECK(!f.state->save(f.plugin, &stream.output));
    stream.fail = false; stream.data[0] = 'X'; stream.position = 0;
    CHECK(!f.state->load(f.plugin, &stream.input)); CHECK(f.get(Interval) == 33);
}

void oldSessionsAreRefused() {
    Fixture f;
    // Sessions from 0.x ("GRDL", four delay lines) must not be misread as macro settings.
    MemoryStream old;
    old.data = {'G','R','D','L', 1,0,0,0, 1,0,0,0, 0,0,0,0, 0,0,0,0,0,0,0,0};
    f.set(Mix, 77);
    CHECK(!f.state->load(f.plugin, &old.input));
    CHECK(f.get(Mix) == 77);
}

void remoteControls() {
    Fixture f;
    auto* remotes = static_cast<const clap_plugin_remote_controls_t*>(f.plugin->get_extension(f.plugin, CLAP_EXT_REMOTE_CONTROLS));
    CHECK(remotes && remotes->count(f.plugin) == 2);
    for (uint32_t page = 0; page < 2; ++page) {
        clap_remote_controls_page_t info {};
        CHECK(remotes->get(f.plugin, page, &info));
        for (auto id : info.param_ids) CHECK(id == CLAP_INVALID_ID || id < ParamCount);
    }
    clap_remote_controls_page_t info {};
    CHECK(!remotes->get(f.plugin, 2, &info));
}

void automationAndInPlace() {
    Fixture full, split;
    for (auto* f : {&full, &split}) { f->set(Mix, 0); f->activate(); }
    full.left.fill(.25f); full.right.fill(-.25f);
    // Let the 20 ms smoothing snap Mix to zero so the comparison below starts from exact dry.
    for (int i = 0; i < 40; ++i) { full.left.fill(.25f); full.right.fill(-.25f); full.process(512); }
    for (int i = 0; i < 40; ++i) { split.left.fill(.25f); split.right.fill(-.25f); split.process(512); }
    full.left.fill(.25f); full.right.fill(-.25f);
    Events event; event.add(Mix, 100, 64); full.process(128, &event);
    split.left.fill(.25f); split.right.fill(-.25f); split.process(64);
    for (int i = 0; i < 64; ++i) CHECK(full.left[i] == .25f && full.right[i] == -.25f);
    split.set(Mix, 100); split.left.fill(.25f); split.right.fill(-.25f); split.process(64);
    for (int i = 0; i < 64; ++i) {
        CHECK(full.left[i+64] == split.left[i]); CHECK(full.right[i+64] == split.right[i]);
    }
    CHECK(full.get(Mix) == 100);
    full.plugin->reset(full.plugin);
    full.left.fill(0); full.right.fill(0); full.process(512);
    for (auto x : full.left) CHECK(x == 0);
    full.plugin->stop_processing(full.plugin); full.plugin->deactivate(full.plugin);
    full.activate(96000); full.process(512);
}

void spectralCharacterThroughTheHost() {
    Fixture f;
    f.set(Character, 1); f.set(Mix, 100); f.set(Shimmer, 100); f.set(Warp, 80); f.set(WarpType, 2);
    f.activate();
    float peak = 0;
    for (int block = 0; block < 400; ++block) {
        for (size_t i = 0; i < 512; ++i) f.left[i] = f.right[i] = static_cast<float>(.3*std::sin((block*512+i)*.03));
        f.process(512);
        for (auto x : f.left) { CHECK(std::isfinite(x)); peak = std::max(peak, std::abs(x)); }
    }
    CHECK(peak > .01f && peak <= .981f);
}

int main() {
    CHECK(clap_entry.init("test")); CHECK(clap_entry.init("test"));
    CHECK(!clap_entry.get_factory("unknown"));
    stateAndParameters(); oldSessionsAreRefused(); remoteControls(); automationAndInPlace(); spectralCharacterThroughTheHost();
    clap_entry.deinit(); clap_entry.deinit();
    std::cout << "CLAP: lifecycle, metadata, text conversion, state, old-session refusal, remote pages, sample-offset automation, and host processing passed\n";
}
