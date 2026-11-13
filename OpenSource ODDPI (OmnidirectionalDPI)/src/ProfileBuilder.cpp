#include "ProfileBuilder.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>

namespace oddpi {

namespace {

    constexpr double kPi = 3.14159265358979323846;

    bool Positive(double v) { return std::isfinite(v) && v > 0; }
    bool NonNegative(double v) { return std::isfinite(v) && v >= 0; }

    std::wstring Fmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        vswprintf_s(buf, fmt, args);
        va_end(args);
        return buf;
    }

    std::wstring Widen(const char* s)
    {
        std::wstring w;
        while (*s) w += wchar_t(static_cast<unsigned char>(*s++));
        return w;
    }

    const wchar_t* AccelModeName(ra::accel_mode mode)
    {
        switch (mode) {
        case ra::accel_mode::classic:     return L"classic";
        case ra::accel_mode::jump:        return L"jump";
        case ra::accel_mode::natural:     return L"natural";
        case ra::accel_mode::synchronous: return L"synchronous";
        case ra::accel_mode::power:       return L"power";
        case ra::accel_mode::lookup:      return L"lookup";
        default:                          return L"none";
        }
    }

    ra::cap_mode ToRaCapMode(CapMode m)
    {
        switch (m) {
        case CapMode::Input:       return ra::cap_mode::in;
        case CapMode::InputOutput: return ra::cap_mode::io;
        default:                   return ra::cap_mode::out;
        }
    }

    ra::accel_mode ToRaAccelMode(DynamicMode m)
    {
        switch (m) {
        case DynamicMode::Classic:     return ra::accel_mode::classic;
        case DynamicMode::Natural:     return ra::accel_mode::natural;
        case DynamicMode::Jump:        return ra::accel_mode::jump;
        case DynamicMode::Power:       return ra::accel_mode::power;
        case DynamicMode::Synchronous: return ra::accel_mode::synchronous;
        default:                       return ra::accel_mode::noaccel;
        }
    }

    const wchar_t* DirectionLabel(int degrees)
    {
        switch (degrees) {
        case 0:   return L"Right";
        case 45:  return L"Up-right";
        case 90:  return L"Up";
        case 135: return L"Up-left";
        case 180: return L"Left";
        case 225: return L"Down-left";
        case 270: return L"Down";
        case 315: return L"Down-right";
        default:  return L"";
        }
    }
}

int EffectiveDpi(const Config& cfg, int driverDefaultDpi)
{
    if (cfg.mouseDpi > 0) return cfg.mouseDpi;
    if (driverDefaultDpi > 0) return driverDefaultDpi;
    return 0;
}

bool BuildProfile(const Config& c, int effectiveDpi, ra::profile& p, std::vector<std::wstring>& errors)
{
    auto require = [&](bool ok, const wchar_t* msg) {
        if (!ok) errors.push_back(msg);
    };

    require(Positive(c.sensitivity), L"[General] Sensitivity must be greater than 0");
    require(std::isfinite(c.rotation), L"[General] Rotation must be a number");
    require(c.angleSnap >= 0 && c.angleSnap <= 45, L"[General] AngleSnap must be between 0 and 45");
    require(c.mouseDpi >= 0, L"[General] MouseDPI can not be negative");
    require(Positive(c.right), L"[Directional] Right must be greater than 0");
    require(Positive(c.left), L"[Directional] Left must be greater than 0");
    require(Positive(c.up), L"[Directional] Up must be greater than 0");
    require(Positive(c.down), L"[Directional] Down must be greater than 0");
    require(Positive(c.blendHorizontal), L"[Directional] BlendHorizontal must be greater than 0");
    require(Positive(c.blendVertical), L"[Directional] BlendVertical must be greater than 0");
    require(NonNegative(c.horizontalStrength), L"[Dynamic] HorizontalStrength can not be negative");
    require(NonNegative(c.verticalStrength), L"[Dynamic] VerticalStrength can not be negative");

    const bool blending = c.blendHorizontal != 1.0 || c.blendVertical != 1.0;
    const bool dynamic = c.mode != DynamicMode::Off;

    require(!(blending && dynamic),
        L"BlendHorizontal/BlendVertical can not be combined with a Dynamic mode "
        L"(Raw Accel uses the same slot for both). Use HorizontalStrength/VerticalStrength, "
        L"or set Mode=Off.");

    if (!errors.empty()) return false;

    p = ra::profile{};
    wcscpy_s(p.name, kProfileName);

    p.degrees_rotation = c.rotation;
    p.degrees_snap = c.angleSnap;
    p.speed_processor_args.whole = true;

    // Directional DPI. In the driver (after the speed curve):
    //   x *= outDPI/1000;          if x < 0 (left):  x *= L/R
    //   y *= outDPI/1000 * Y/X;    if y < 0 (up):    y *= U/D
    // so Right is the base and the other three are ratios on top of it.
    // When a mouse DPI is set the driver also multiplies by 1000/DPI, so start from the
    // mouse's own DPI to make 1.0 mean "unchanged".
    const double baseDpi = effectiveDpi > 0 ? double(effectiveDpi) : ra::NORMALIZED_DPI;
    p.output_dpi = baseDpi * c.sensitivity * c.right;
    p.lr_output_dpi_ratio = c.left / c.right;
    p.yx_output_dpi_ratio = c.down / c.right;
    p.ud_output_dpi_ratio = c.up / c.down;

    auto& a = p.accel_x;

    if (!dynamic) {
        if (blending) {
            // Smooth horizontal -> vertical blend, direction-preserving.
            // The driver computes  scale = 1 + (f(speed) - 1) * weight(angle),
            // with weight linear from range_weights.x (0 deg) to range_weights.y (90 deg).
            // A constant lookup curve f = q with weights {0, 1} gives
            //   scale = 1 + (q - 1) * angle / 90deg    ->  1 at horizontal, q at vertical.
            // BlendHorizontal itself is folded into the output DPI.
            p.output_dpi *= c.blendHorizontal;
            const double q = c.blendVertical / c.blendHorizontal;

            if (q != 1.0) {
                a.mode = ra::accel_mode::lookup;
                a.gain = false;  // table value is the sensitivity itself
                a.length = 4;    // 2 points = 4 floats: (1, q), (2, q)
                a.data[0] = 1.0f;
                a.data[1] = static_cast<float>(q);
                a.data[2] = 2.0f;
                a.data[3] = static_cast<float>(q);
                p.range_weights = { 0.0, 1.0 };
            }
        }
    }
    else {
        a.mode = ToRaAccelMode(c.mode);
        a.gain = c.gain;
        a.acceleration = c.acceleration;
        a.exponent_classic = c.exponent;
        a.input_offset = c.inputOffset;
        a.output_offset = c.outputOffset;
        a.decay_rate = c.decayRate;
        a.limit = c.limit;
        a.scale = c.scale;
        a.exponent_power = c.powerExponent;
        a.gamma = c.gamma;
        a.motivity = c.motivity;
        a.sync_speed = c.syncSpeed;
        a.smooth = c.smooth;
        a.cap = { c.capInput, c.capOutput };
        a.cap_mode = ToRaCapMode(c.capMode);
        p.range_weights = { c.horizontalStrength, c.verticalStrength };
    }

    ra::valid(p, [&](auto msg) {
        errors.push_back(L"Raw Accel rejected the settings: " + Widen(msg));
    });

    return errors.empty();
}

struct PreviewModel::Impl {
    // modifier_settings is large (lookup tables), keep it off the stack.
    std::unique_ptr<ra::modifier_settings> settings = std::make_unique<ra::modifier_settings>();
    std::unique_ptr<ra::modifier> mod;
    double dpiFactor = 1.0;
};

PreviewModel::PreviewModel(const ra::profile& profile, int effectiveDpi)
    : impl_(new Impl)
{
    impl_->settings->prof = profile;
    ra::init_data(*impl_->settings);
    impl_->mod = std::make_unique<ra::modifier>(*impl_->settings);
    // Same normalisation the driver applies per device.
    impl_->dpiFactor = effectiveDpi > 0 ? ra::NORMALIZED_DPI / effectiveDpi : 1.0;
}

PreviewModel::~PreviewModel() { delete impl_; }

PreviewModel::Point PreviewModel::At(double degrees, double speed) const
{
    const double rad = degrees * kPi / 180.0;
    const double counts = speed / impl_->dpiFactor;
    vec2d in = { counts * std::cos(rad), -counts * std::sin(rad) };  // screen y points down
    vec2d out = in;

    ra::speed_processor sp;
    sp.init(impl_->settings->prof.speed_processor_args);
    impl_->mod->modify(out, sp, *impl_->settings, impl_->dpiFactor, 1.0);

    double angle = std::atan2(-out.y, out.x) * 180.0 / kPi;
    if (angle < 0) angle += 360.0;
    if (angle >= 359.95) angle = 0.0;
    return { angle, magnitude(out) / magnitude(in) };
}

void PrintPreview(const Config& cfg, int effectiveDpi, const ra::profile& profile)
{
    const PreviewModel model(profile, effectiveDpi);
    const bool dynamic = cfg.mode != DynamicMode::Off;
    const wchar_t* unit = effectiveDpi > 0 ? L"in/s" : L"c/ms";

    std::vector<double> speeds = dynamic ? std::vector<double>{ 2, 10, 30, 60 }
                                         : std::vector<double>{ 10 };

    auto simulate = [&](double degrees, double speed) { return model.At(degrees, speed); };

    wprintf(L"Raw Accel profile: %ls\n\n", DescribeProfile(profile).c_str());
    wprintf(L"  Hand movement           Cursor     Sensitivity");
    if (dynamic) {
        wprintf(L" at speed (%ls):\n  %-23ls %-10ls", unit, L"", L"angle");
        for (double s : speeds) wprintf(L" %8.0f", s);
    }
    else {
        wprintf(L"\n  %-23ls %-10ls", L"", L"angle");
    }
    wprintf(L"\n  ---------------------   --------   ");
    for (size_t i = 0; i < speeds.size(); ++i) wprintf(L"---------");
    wprintf(L"\n");

    for (int deg = 0; deg < 360; deg += 15) {
        wprintf(L"  %-10ls %6d deg   %6.1f deg ", DirectionLabel(deg), deg, simulate(deg, speeds[0]).angle);
        for (double s : speeds) wprintf(L" %7.3fx", simulate(deg, s).sens);
        wprintf(L"\n");
    }

    if (std::fabs(cfg.rotation) > 0) {
        wprintf(L"\n  Note: Rotation turns every movement by %.1f deg before the other settings apply.\n", cfg.rotation);
    }
}

std::wstring DescribeProfile(const ra::profile& p)
{
    std::wstring s = Fmt(L"\"%ls\"  outDPI %.0f, Y/X %.3f, L/R %.3f, U/D %.3f, rotation %.1f, snap %.1f, curve %ls",
        p.name, p.output_dpi, p.yx_output_dpi_ratio, p.lr_output_dpi_ratio, p.ud_output_dpi_ratio,
        p.degrees_rotation, p.degrees_snap, AccelModeName(p.accel_x.mode));

    if (p.range_weights.x != 1.0 || p.range_weights.y != 1.0) {
        s += Fmt(L", range weights %.2f/%.2f", p.range_weights.x, p.range_weights.y);
    }
    return s;
}

}  // namespace oddpi
