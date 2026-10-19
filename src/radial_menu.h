#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace radial {
constexpr float Pi = 3.14159265358979323846f;
enum class Action { Text, Multimedia, Gif, File, Voice, Stickers };
enum class Mode { NewMessage, Reply };
struct Config {
    int64_t hold_us = 250000, open_us = 350000, close_us = 180000, stagger_us = 30000;
    float x = 480, y = 272, outside_x = 1230, inner_radius = 112, outer_radius = 248;
    float petal_gap = 5, corner_radius = 11, dead_zone = 48, neutral_zone = 32, hysteresis_degrees = 6;
};
struct Context {
    int64_t chat = 0, thread = 0, selected = 0, reply = 0;
    int32_t topic = 0;
    bool samePlace(const Context& other) const {
        return chat == other.chat && topic == other.topic && thread == other.thread;
    }
};
struct Input {
    bool allowed = false, held = false, pressed = false, circle = false, left = false, right = false, interrupted = false;
    int stick_x = 128, stick_y = 128;
    Context context;
};
struct Visual {
    float x = 1100, y = 272, opacity = 0;
    std::array<float, 5> petals{};
};
struct Result {
    bool consume = false, execute = false;
    Action action = Action::Text;
    Mode mode = Mode::NewMessage;
    Context context;
    int64_t reply = 0;
};
struct Point { float x, y; };
// Rounded contours and circles are built once; frames only transform cached points.
struct Geometry {
    static constexpr int ArcSteps = 40, CornerSteps = 12, CircleSteps = 192;
    static constexpr int Points = 2 * (ArcSteps + 1) + 4 * CornerSteps + 2;
    static constexpr int Layers = 6;
    static constexpr std::array<float, Layers> Expansion = {10, 6, 3, 1.1f, 0, -1.1f};
    using Contour = std::array<Point, Points>;
    std::array<std::array<Contour, Layers>, 5> petals{};
    std::array<Point, 5> centers{};
    std::array<Contour, 5> normals{};
    std::array<Point, CircleSteps + 1> circle{};
    explicit Geometry(const Config& config) {
        const float middle = (config.inner_radius + config.outer_radius) / 2;
        for (int i = 0; i <= CircleSteps; ++i) circle[i] = polar(1, 2 * Pi * i / CircleSteps);
        for (int petal = 0; petal < 5; ++petal) {
            const float angle = (-90 + petal * 72) * Pi / 180;
            centers[petal] = polar(middle, angle);
            for (int layer = 0; layer < Layers; ++layer) {
                const float expansion = Expansion[layer];
                const float inner = config.inner_radius - expansion, outer = config.outer_radius + expansion;
                const float round = config.corner_radius + expansion;
                const float start = angle - Pi / 5, end = angle + Pi / 5;
                const float gap = config.petal_gap / 2 - expansion;
                const float outer_start = start + gap / outer, outer_end = end - gap / outer;
                const float inner_start = start + gap / inner, inner_end = end - gap / inner;
                auto& contour = petals[petal][layer];
                int point = 0;
                auto add = [&](Point value) { contour[point++] = {value.x - centers[petal].x, value.y - centers[petal].y}; };
                auto arc = [&](float radius, float from, float to) {
                    for (int step = 0; step <= ArcSteps; ++step) add(polar(radius, from + (to - from) * step / ArcSteps));
                };
                auto corner = [&](Point from, Point control, Point to) {
                    for (int step = 1; step <= CornerSteps; ++step) {
                        const float t = step / (float)CornerSteps, u = 1 - t;
                        add({u*u*from.x + 2*u*t*control.x + t*t*to.x, u*u*from.y + 2*u*t*control.y + t*t*to.y});
                    }
                };
                arc(outer, outer_start + round / outer, outer_end - round / outer);
                corner(polar(outer, outer_end - round / outer), polar(outer, outer_end), polar(outer - round, outer_end));
                add(polar(inner + round, inner_end));
                corner(polar(inner + round, inner_end), polar(inner, inner_end), polar(inner, inner_end - round / inner));
                arc(inner, inner_end - round / inner, inner_start + round / inner);
                corner(polar(inner, inner_start + round / inner), polar(inner, inner_start), polar(inner + round, inner_start));
                add(polar(outer - round, outer_start));
                corner(polar(outer - round, outer_start), polar(outer, outer_start), polar(outer, outer_start + round / outer));
            }
            const auto& contour = petals[petal][4];
            for (int i = 0; i < Points; ++i) {
                const auto before = contour[(i+Points-1) % Points], after = contour[(i+1) % Points];
                const float dx = after.x-before.x, dy = after.y-before.y;
                const float length = std::sqrt(dx*dx+dy*dy);
                normals[petal][i] = length > 0 ? Point{dy/length,-dx/length} : Point{0,0};
            }
        }
    }
private:
    static Point polar(float radius, float angle) { return {radius * std::cos(angle), radius * std::sin(angle)}; }
};
class Menu {
public:
    explicit Menu(Config config = {}) : config_(config) {}
    bool busy() const { return stage_ != Stage::Idle; }
    bool visible() const { return stage_ == Stage::Open || stage_ == Stage::Closing; }
    Action action() const { return action_; }
    Mode mode() const { return mode_; }
    int64_t reply() const { return target_; }
    const Context& context() const { return context_; }
    const Config& config() const { return config_; }
    Visual visual(int64_t now) const {
        Visual view;
        view.x = config_.outside_x; view.y = config_.y;
        if (stage_ == Stage::Open) {
            const float slide = ease(fraction(now - opened_, config_.open_us));
            view.x += (config_.x - config_.outside_x) * slide;
            view.opacity = slide;
            for (int i = 0; i < 5; ++i)
                view.petals[i] = ease(fraction(now - opened_ - i * config_.stagger_us,
                                              config_.open_us - 4 * config_.stagger_us));
        } else if (stage_ == Stage::Closing) {
            const float fold = 1 - ease(fraction(now - closed_, config_.close_us));
            view.x += (closing_.x - config_.outside_x) * fold;
            view.opacity = closing_.opacity * fold;
            for (int i = 0; i < 5; ++i) view.petals[i] = closing_.petals[i] * fold;
        }
        return view;
    }
    Result update(const Input& input, int64_t now) {
        Result result;
        result.consume = busy();
        if (stage_ == Stage::Idle) {
            if (!input.allowed || !input.pressed || input.interrupted || input.circle) return result;
            stage_ = Stage::Pending; pressed_ = now; last_ = now;
            context_ = input.context; target_ = context_.reply ? context_.reply : context_.selected;
            mode_ = context_.reply ? Mode::Reply : Mode::NewMessage;
            action_ = Action::Text; result.consume = true;
        }
        const bool cancelled = !input.allowed || !context_.samePlace(input.context) || input.circle ||
            input.interrupted || now < last_ || now - last_ > 1000000;
        last_ = now;
        if (stage_ == Stage::Blocked) {
            if (!input.held) stage_ = Stage::Idle;
            return result;
        }
        if (stage_ == Stage::Closing) {
            if (cancelled || input.pressed) confirm_ = false;
            if (now - closed_ >= config_.close_us) {
                if (confirm_) result = confirmed();
                stage_ = input.held ? Stage::Blocked : Stage::Idle;
            }
            return result;
        }
        if (cancelled) {
            if (stage_ == Stage::Open) close(now, false);
            else stage_ = input.held ? Stage::Blocked : Stage::Idle;
            return result;
        }
        if (input.left != input.right && target_ > 0)
            mode_ = mode_ == Mode::NewMessage ? Mode::Reply : Mode::NewMessage;
        if (stage_ == Stage::Pending && now - pressed_ >= config_.hold_us) {
            stage_ = Stage::Open; opened_ = pressed_ + config_.hold_us;
        }
        if (stage_ == Stage::Open) select(input.stick_x, input.stick_y);
        if (!input.held) {
            if (stage_ == Stage::Pending) { result = confirmed(); stage_ = Stage::Idle; }
            else close(now, true);
        }
        return result;
    }
private:
    enum class Stage { Idle, Pending, Open, Closing, Blocked };
    Config config_;
    Stage stage_ = Stage::Idle;
    Action action_ = Action::Text;
    Mode mode_ = Mode::NewMessage;
    Context context_;
    int64_t target_ = 0, pressed_ = 0, opened_ = 0, closed_ = 0, last_ = 0;
    bool confirm_ = false;
    Visual closing_;
    static float fraction(int64_t time, int64_t duration) { return std::clamp(time / (float)std::max<int64_t>(1, duration), 0.f, 1.f); }
    static float ease(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
    Result confirmed() const { return {true, true, action_, mode_, context_, mode_ == Mode::Reply ? target_ : 0}; }
    void close(int64_t now, bool confirm) {
        closing_ = visual(now); closed_ = now; confirm_ = confirm; stage_ = Stage::Closing;
    }
    void select(int x, int y) {
        const float dx = x - 128.f, dy = y - 128.f;
        const float distance = dx * dx + dy * dy;
        if (distance < config_.neutral_zone * config_.neutral_zone) { action_ = Action::Text; return; }
        if (action_ == Action::Text && distance < config_.dead_zone * config_.dead_zone) return;
        float angle = std::atan2(dy, dx) * 180 / Pi + 90;
        if (angle < 0) angle += 360;
        if (action_ != Action::Text) {
            float difference = std::fabs(angle - ((int)action_ - 1) * 72);
            difference = std::min(difference, 360 - difference);
            if (difference <= 36 + config_.hysteresis_degrees) return;
        }
        action_ = (Action)(1 + (int)((angle + 36) / 72) % 5);
    }
};
} // namespace radial
