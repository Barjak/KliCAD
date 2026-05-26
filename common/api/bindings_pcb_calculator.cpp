/*
 * KliCAD subsystem binding: pcb_calculator (pure-math subset).
 *
 * Module: klicad_native_pcb_calculator
 *
 * KiCad's PCB Calculator is implemented as a kiface (pcb_calculator_kiface)
 * with a wxWidgets frame.  This binding exposes only the *math* layer —
 * none of the panels, frames, or dialogs.  It is Pattern A (libkicommon-
 * resident), so it gets registered before the embedded interpreter starts,
 * and is importable from any RunPython context without spawning the
 * calculator GUI.
 *
 * Implementation note: the math classes (ESERIES, RES_EQUIV_CALC,
 * ATTENUATOR*, TRANSLINE*, IEC60664, panel-private helpers) live in
 * pcb_calculator_kiface (kiface DSO) and in libcommon (transline_calculations
 * subdir).  None of those symbols are linkable from libkicommon, and per the
 * binding brief we are not allowed to extend kicommon's link surface or to
 * touch pcb_calculator's CMakeLists.  Therefore each function below
 * reimplements the formula inline, with a header comment pointing back to
 * the canonical source class so the two stay in sync if the upstream math
 * is ever revised.  The formulas themselves are standardised (IEC 60063,
 * IPC-2221, Qucs attenuator synthesis) and the pcb_calculator
 * implementations are already documented in pcb_calculator/*.h and
 * pcb_calculator/calculator_panels/*.cpp.
 *
 * Functions:
 *   e_series_values(series='E24')                 -> list[float]
 *       Mirrors ESERIES::ESERIES_VALUES + ESERIES_IN_DECADE
 *       (pcb_calculator/eseries.{h,cpp}).
 *
 *   e_series_closest(target, series='E24')        -> dict
 *       Closest single value in the requested series across all decades.
 *
 *   resistor_combination(target, series='E24')    -> dict
 *       Best 2R / 3R combinations of E-series resistors to hit target.
 *       Mirrors the 2R/3R combinatorics in RES_EQUIV_CALC
 *       (pcb_calculator/resistor_substitution_utils.{h,cpp}).  Limited to
 *       E1..E24 by upstream design (calculation cost on E48+ is impractical).
 *
 *   attenuator(kind, attenuation_db, z_in, z_out=None) -> dict
 *       Mirrors ATTENUATOR_PI / _TEE / _BRIDGE / _SPLITTER ::Calculate()
 *       (pcb_calculator/attenuators/attenuator_classes.{h,cpp}).
 *
 *   trace_width_for_current(...)                  -> dict
 *       IPC-2221 formula (PANEL_TRACK_WIDTH::TWCalculateWidth /
 *       TWCalculateCurrent in pcb_calculator/calculator_panels/
 *       panel_track_width.cpp).
 *
 *   fusing_current(width_mm, thickness_oz, ...)   -> dict
 *       Mirrors PANEL_FUSING_CURRENT::m_onCalculateClick
 *       (pcb_calculator/calculator_panels/panel_fusing_current.cpp).
 *
 * Skipped (cannot be reached from libkicommon without CMakeLists changes
 * the brief forbids):
 *   - Transline analysis/synthesis (MICROSTRIP, STRIPLINE, COAX, ...).  The
 *     calculator math is in common/transline_calculations/*.cpp, which is
 *     linked into libcommon (static) rather than libkicommon.  Adding it
 *     to KICOMMON_SRCS or linking common into kicommon would be the
 *     prerequisite — but both are CMakeLists changes the brief disallows.
 *     Re-implementing Hammerstad-Jensen et al. inline would not be a
 *     trustworthy substitute for the upstream class, so we skip rather
 *     than ship an inferior copy.
 *   - IEC60664 spacing.  Lookup tables and rule chain live in
 *     pcb_calculator/calculator_panels/iec60664.{h,cpp} — kiface-only.
 *     Same reachability story.
 *   - Resistor colour-code decoding.  The colour tables live inside
 *     PANEL_COLOR_CODE.  No standalone math class.  Easy to add but
 *     would need a private copy of the colour table; deferred until it
 *     earns its keep.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

// ===========================================================================
// E-series tables.  Mirrors ESERIES::ESERIES_VALUES (pcb_calculator/eseries.cpp).
// Values are stored in the [100, 999] decade as integers so all values are
// representable precisely.  Same layout, same skip-step logic as upstream.
// ===========================================================================

enum E_SERIES_ID
{
    PCB_CALC_E1 = 0,
    PCB_CALC_E3,
    PCB_CALC_E6,
    PCB_CALC_E12,
    PCB_CALC_E24,
    PCB_CALC_E48,
    PCB_CALC_E96,
    PCB_CALC_E192,
};


const std::vector<uint16_t>& pcb_calc_e24_table()
{
    static const std::vector<uint16_t> t = {
        100, 110, 120, 130, 150, 160, 180, 200, 220, 240, 270, 300,
        330, 360, 390, 430, 470, 510, 560, 620, 680, 750, 820, 910
    };
    return t;
}


const std::vector<uint16_t>& pcb_calc_e192_table()
{
    static const std::vector<uint16_t> t = {
        100, 101, 102, 104, 105, 106, 107, 109, 110, 111, 113, 114, 115, 117, 118, 120, 121, 123,
        124, 126, 127, 129, 130, 132, 133, 135, 137, 138, 140, 142, 143, 145, 147, 149, 150, 152,
        154, 156, 158, 160, 162, 164, 165, 167, 169, 172, 174, 176, 178, 180, 182, 184, 187, 189,
        191, 193, 196, 198, 200, 203, 205, 208, 210, 213, 215, 218, 221, 223, 226, 229, 232, 234,
        237, 240, 243, 246, 249, 252, 255, 258, 261, 264, 267, 271, 274, 277, 280, 284, 287, 291,
        294, 298, 301, 305, 309, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 357, 361,
        365, 370, 374, 379, 383, 388, 392, 397, 402, 407, 412, 417, 422, 427, 432, 437, 442, 448,
        453, 459, 464, 470, 475, 481, 487, 493, 499, 505, 511, 517, 523, 530, 536, 542, 549, 556,
        562, 569, 576, 583, 590, 597, 604, 612, 619, 626, 634, 642, 649, 657, 665, 673, 681, 690,
        698, 706, 715, 723, 732, 741, 750, 759, 768, 777, 787, 796, 806, 816, 825, 835, 845, 856,
        866, 876, 887, 898, 909, 920, 931, 942, 953, 965, 976, 988
    };
    return t;
}


int pcb_calc_e_series_from_string( const std::string& aName )
{
    // Accept E1, E3, E6, E12, E24, E48, E96, E192 (case-insensitive, optional
    // leading 'E').  Throws std::invalid_argument on unknown values.
    std::string up;
    up.reserve( aName.size() );
    for( char c : aName )
        up.push_back( static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) ) );

    if( !up.empty() && up.front() == 'E' )
        up.erase( up.begin() );

    if( up == "1" )   return PCB_CALC_E1;
    if( up == "3" )   return PCB_CALC_E3;
    if( up == "6" )   return PCB_CALC_E6;
    if( up == "12" )  return PCB_CALC_E12;
    if( up == "24" )  return PCB_CALC_E24;
    if( up == "48" )  return PCB_CALC_E48;
    if( up == "96" )  return PCB_CALC_E96;
    if( up == "192" ) return PCB_CALC_E192;

    throw std::invalid_argument( "unknown E-series '" + aName
                                 + "' — expected E1/E3/E6/E12/E24/E48/E96/E192" );
}


// Build the [100, 999] decade slice for the named series.  Same algorithm as
// ESERIES_VALUES::ESERIES_VALUES — pick every Nth entry from the base table.
std::vector<uint16_t> pcb_calc_series_base_values( int aSeries )
{
    const std::vector<uint16_t>* base = nullptr;
    unsigned int                 skip = 1;

    if( aSeries <= PCB_CALC_E24 )
    {
        // E1..E24 derive from the E24 table.  Skip values mirror upstream:
        // E24=1, E12=2, E6=4, E3=8, E1=24.
        static const unsigned int skipE124[] = { 24, 8, 4, 2, 1 };
        base = &pcb_calc_e24_table();
        skip = skipE124[aSeries];
    }
    else
    {
        // E48/E96/E192 derive from the E192 table.  Skip = 2^(E192 - series).
        base = &pcb_calc_e192_table();
        skip = 1u << ( PCB_CALC_E192 - aSeries );
    }

    std::vector<uint16_t> out;
    out.reserve( base->size() / skip );
    for( size_t i = 0; i < base->size(); i += skip )
        out.push_back( ( *base )[i] );
    return out;
}


// All E-series values across [decadeMin, decadeMax], inclusive, in ohms.
// Mirrors how RES_EQUIV_CALC walks the decades in resistor_substitution_utils.cpp.
std::vector<double> pcb_calc_series_all_decades( int aSeries, int aDecadeMin, int aDecadeMax )
{
    std::vector<uint16_t> base = pcb_calc_series_base_values( aSeries );
    if( base.empty() )
        return {};

    std::vector<double> out;
    out.reserve( base.size() * ( aDecadeMax - aDecadeMin + 1 ) );

    const uint16_t firstBase = base.front();   // 100 for both tables

    for( int dec = aDecadeMin; dec <= aDecadeMax; ++dec )
    {
        double mult = std::pow( 10.0, dec );
        for( uint16_t v : base )
            out.push_back( mult * static_cast<double>( v ) / static_cast<double>( firstBase ) );
    }
    return out;
}


// ===========================================================================
// Public binding: list E-series values in a single decade [1, 10).
// ===========================================================================

py::list e_series_values( const std::string& aSeries )
{
    int                   series = pcb_calc_e_series_from_string( aSeries );
    std::vector<uint16_t> base   = pcb_calc_series_base_values( series );

    py::list out;
    if( base.empty() )
        return out;

    double first = static_cast<double>( base.front() );
    for( uint16_t v : base )
        out.append( static_cast<double>( v ) / first );

    return out;
}


// ===========================================================================
// Public binding: closest single E-series value to a target.
// ===========================================================================

py::dict e_series_closest( double aTarget, const std::string& aSeries )
{
    if( !( aTarget > 0.0 ) || !std::isfinite( aTarget ) )
        throw std::invalid_argument( "e_series_closest: target must be positive and finite" );

    int                   series = pcb_calc_e_series_from_string( aSeries );
    std::vector<uint16_t> base   = pcb_calc_series_base_values( series );
    if( base.empty() )
        throw std::runtime_error( "e_series_closest: empty series table" );

    // Reduce target to the [first, last) interval of the base table by
    // factoring out the appropriate power of 10.  This avoids enumerating
    // every decade.
    double first = static_cast<double>( base.front() );
    double last  = static_cast<double>( base.back() );

    int    decade  = 0;
    double reduced = aTarget;
    // Keep multiplying/dividing by 10 to land in [first, first*10).
    while( reduced < first )
    {
        reduced *= 10.0;
        --decade;
    }
    while( reduced >= first * 10.0 )
    {
        reduced /= 10.0;
        ++decade;
    }

    double bestRatioErr = std::numeric_limits<double>::infinity();
    double bestVal      = 0.0;

    // Consider this decade and the one above (since the closest may straddle
    // the decade boundary, e.g. 950 vs 1000 for E12).
    for( int decOff : { 0, 1 } )
    {
        double mult = std::pow( 10.0, decOff );
        for( uint16_t v : base )
        {
            double cand = static_cast<double>( v ) * mult;
            // Geometric error to handle multi-decade range uniformly.
            double err = std::abs( std::log( cand / reduced ) );
            if( err < bestRatioErr )
            {
                bestRatioErr = err;
                bestVal      = cand;
            }
        }
        (void) last;
    }

    // Scale back to the original decade.
    double scaledBestVal = bestVal * std::pow( 10.0, decade ) / first;
    double errPct        = ( scaledBestVal - aTarget ) / aTarget * 100.0;

    py::dict result;
    result["value"]     = scaledBestVal;
    result["error_pct"] = errPct;
    result["series"]    = aSeries;
    result["decade"]    = decade;
    return result;
}


// ===========================================================================
// Public binding: best 2R / 3R combination from an E-series.  Mirrors
// RES_EQUIV_CALC's 2R/3R passes in pcb_calculator/resistor_substitution_utils.cpp.
// Upstream caps at E24 because the 3R/4R search space explodes on E48+ — we
// keep the same cap.  Only 1R, 2R-series, 2R-parallel and best 3R are
// considered here (4R is omitted; upstream's calculate4RSolution depends on
// a sorted 2R buffer that is expensive to build).
// ===========================================================================

py::dict resistor_combination( double aTarget, const std::string& aSeries )
{
    if( !( aTarget > 0.0 ) || !std::isfinite( aTarget ) )
        throw std::invalid_argument( "resistor_combination: target must be positive and finite" );

    int series = pcb_calc_e_series_from_string( aSeries );
    if( series > PCB_CALC_E24 )
        throw std::invalid_argument(
                "resistor_combination: series larger than E24 is not supported (matches "
                "RES_EQUIV_CALC upstream limit — search space is impractical)" );

    // Upstream RES_EQUIV_CALC limits the resistor range to [10, 1e6] Ohms.
    // We do the same.
    std::vector<double> values = pcb_calc_series_all_decades( series, 1, 6 );
    if( values.empty() )
        throw std::runtime_error( "resistor_combination: no values for series" );

    auto better = [&]( double aCand, double aBest )
    {
        return std::abs( aCand - aTarget ) < std::abs( aBest - aTarget );
    };

    // ---- 1R ----
    double singleBest = values.front();
    for( double v : values )
        if( better( v, singleBest ) )
            singleBest = v;

    // ---- 2R series (R1 + R2) and 2R parallel ----
    double pairBest    = singleBest;
    double pairR1      = singleBest;
    double pairR2      = 0.0;
    std::string pairOp = "single";

    for( double a : values )
    {
        // Series: a + b.  Since values is sorted ascending, we can break early
        // once a + b exceeds 2 * target.
        for( double b : values )
        {
            double s = a + b;
            if( s > 2.0 * aTarget && s > pairBest )
                break;
            if( better( s, pairBest ) )
            {
                pairBest = s;
                pairR1   = a;
                pairR2   = b;
                pairOp   = "series";
            }
        }
        // Parallel: a*b/(a+b)
        for( double b : values )
        {
            double p = ( a * b ) / ( a + b );
            if( p < aTarget * 0.5 )
                continue;
            if( better( p, pairBest ) )
            {
                pairBest = p;
                pairR1   = a;
                pairR2   = b;
                pairOp   = "parallel";
            }
        }
    }

    // ---- 3R: (a + b) || c   and   a + (b || c) ----
    // Bounded scan; upstream RES_EQUIV_CALC also walks the 2R buffer.
    double tripleBest          = pairBest;
    double tripleR1            = pairR1;
    double tripleR2            = pairR2;
    double tripleR3            = 0.0;
    std::string tripleOp       = pairOp;

    for( double a : values )
    {
        for( double b : values )
        {
            double seriesAB   = a + b;
            double parallelAB = ( a * b ) / ( a + b );

            // Skip if the 2R intermediate is far enough from target that no
            // 3rd resistor can rescue it within current best.  Coarse prune.
            if( seriesAB > 10.0 * aTarget && parallelAB > 10.0 * aTarget )
                continue;

            for( double c : values )
            {
                // (a+b) || c
                double v1 = ( seriesAB * c ) / ( seriesAB + c );
                if( better( v1, tripleBest ) )
                {
                    tripleBest = v1;
                    tripleR1   = a;
                    tripleR2   = b;
                    tripleR3   = c;
                    tripleOp   = "(R1+R2)||R3";
                }
                // a + (b || c)
                double v2 = a + ( b * c ) / ( b + c );
                if( better( v2, tripleBest ) )
                {
                    tripleBest = v2;
                    tripleR1   = a;
                    tripleR2   = b;
                    tripleR3   = c;
                    tripleOp   = "R1+(R2||R3)";
                }
            }
        }
    }

    auto build_dict = [&]( const std::string& aOp, double aValue, double aR1, double aR2,
                           double aR3 )
    {
        py::dict d;
        d["op"]        = aOp;
        d["value"]     = aValue;
        d["error_pct"] = ( aValue - aTarget ) / aTarget * 100.0;
        d["r1"]        = aR1;
        d["r2"]        = aR2;
        if( aR3 > 0.0 )
            d["r3"] = aR3;
        return d;
    };

    py::dict out;
    out["target"]  = aTarget;
    out["series"]  = aSeries;
    out["single"]  = build_dict( "single", singleBest, singleBest, 0.0, 0.0 );
    out["pair"]    = build_dict( pairOp, pairBest, pairR1, pairR2, 0.0 );
    out["triple"]  = build_dict( tripleOp, tripleBest, tripleR1, tripleR2, tripleR3 );
    return out;
}


// ===========================================================================
// Attenuator synthesis.  Mirrors ATTENUATOR_PI/_TEE/_BRIDGE/_SPLITTER ::Calculate()
// in pcb_calculator/attenuators/attenuator_classes.cpp.
// ===========================================================================

enum PCB_CALC_ATTEN_KIND
{
    PCB_CALC_ATTEN_PI,
    PCB_CALC_ATTEN_TEE,
    PCB_CALC_ATTEN_BRIDGED_TEE,
    PCB_CALC_ATTEN_SPLITTER,
};


int pcb_calc_atten_kind_from_string( const std::string& aKind )
{
    std::string k;
    k.reserve( aKind.size() );
    for( char c : aKind )
        k.push_back( static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) ) );

    if( k == "pi" )                                       return PCB_CALC_ATTEN_PI;
    if( k == "tee" || k == "t" )                          return PCB_CALC_ATTEN_TEE;
    if( k == "bridged_tee" || k == "bridge" || k == "bridged" )
        return PCB_CALC_ATTEN_BRIDGED_TEE;
    if( k == "split" || k == "splitter" )                 return PCB_CALC_ATTEN_SPLITTER;

    throw std::invalid_argument( "unknown attenuator kind '" + aKind
                                 + "' — expected pi/tee/bridged_tee/split" );
}


py::dict attenuator( const std::string& aKind, double aAttenDb, double aZin,
                     std::optional<double> aZout )
{
    int   kind = pcb_calc_atten_kind_from_string( aKind );
    double Zout = aZout.value_or( aZin );

    if( !( aZin > 0.0 ) || !std::isfinite( aZin ) )
        throw std::invalid_argument( "attenuator: z_in must be positive and finite" );
    if( !( Zout > 0.0 ) || !std::isfinite( Zout ) )
        throw std::invalid_argument( "attenuator: z_out must be positive and finite" );

    // Splitter and bridge force z_in == z_out (upstream does this in ::Calculate()).
    if( kind == PCB_CALC_ATTEN_SPLITTER || kind == PCB_CALC_ATTEN_BRIDGED_TEE )
        aZin = Zout;

    // L = 10^(A/10).  A = (L+1)/(L-1).  Lmin = minimum-attenuation power ratio for
    // mismatched ends.  All from ATTENUATOR::Calculate() upstream.
    double L    = std::pow( 10.0, aAttenDb / 10.0 );
    double A    = ( L + 1.0 ) / ( L - 1.0 );

    double Lmin;
    if( aZin > Zout )
        Lmin = ( 2.0 * aZin / Zout ) - 1.0 + 2.0 * std::sqrt( aZin / Zout * ( aZin / Zout - 1.0 ) );
    else
        Lmin = ( 2.0 * Zout / aZin ) - 1.0 + 2.0 * std::sqrt( Zout / aZin * ( Zout / aZin - 1.0 ) );

    double minAttDb = 10.0 * std::log10( Lmin );

    py::dict result;
    result["kind"]          = aKind;
    result["atten_db"]      = aAttenDb;
    result["z_in"]          = aZin;
    result["z_out"]         = Zout;
    result["min_atten_db"]  = minAttDb;

    if( kind == PCB_CALC_ATTEN_SPLITTER )
    {
        // ATTENUATOR_SPLITTER::Calculate() — fixed 6 dB three-way splitter,
        // all three legs = Zout / 3.
        double R = Zout / 3.0;
        result["ok"]       = true;
        result["atten_db"] = 6.0;
        result["r1"]       = R;
        result["r2"]       = R;
        result["r3"]       = R;
        return result;
    }

    if( minAttDb > aAttenDb )
    {
        result["ok"]    = false;
        result["error"] = "requested attenuation below minimum for given impedance mismatch";
        return result;
    }

    switch( kind )
    {
    case PCB_CALC_ATTEN_PI:
    {
        // ATTENUATOR_PI::Calculate()
        double R2 = ( ( L - 1.0 ) / 2.0 ) * std::sqrt( aZin * Zout / L );
        double R1 = 1.0 / ( ( A / aZin ) - ( 1.0 / R2 ) );
        double R3 = 1.0 / ( ( A / Zout ) - ( 1.0 / R2 ) );
        result["ok"] = true;
        result["r1"] = R1;
        result["r2"] = R2;
        result["r3"] = R3;
        break;
    }
    case PCB_CALC_ATTEN_TEE:
    {
        // ATTENUATOR_TEE::Calculate()
        double R2 = ( 2.0 * std::sqrt( L * aZin * Zout ) ) / ( L - 1.0 );
        double R1 = aZin * A - R2;
        double R3 = Zout * A - R2;
        result["ok"] = true;
        result["r1"] = R1;
        result["r2"] = R2;
        result["r3"] = R3;
        break;
    }
    case PCB_CALC_ATTEN_BRIDGED_TEE:
    {
        // ATTENUATOR_BRIDGE::Calculate().  Note upstream recomputes L for
        // the bridged topology using A/20 instead of A/10.
        double Lb = std::pow( 10.0, aAttenDb / 20.0 );
        double R1 = aZin * ( Lb - 1.0 );
        double R2 = aZin / ( Lb - 1.0 );
        result["ok"] = true;
        result["r1"] = R1;
        result["r2"] = R2;
        break;
    }
    default:
        throw std::logic_error( "unreachable attenuator kind" );
    }

    return result;
}


// ===========================================================================
// IPC-2221 trace width vs current (external/internal copper layers).
// Mirrors PANEL_TRACK_WIDTH::TWCalculateWidth / TWCalculateCurrent in
// pcb_calculator/calculator_panels/panel_track_width.cpp.
//
//   Imax = scale * dT^0.44 * (W_mil * T_mil)^0.725
//     scale = 0.048 (external) or 0.024 (internal)
//
// Copper thickness in oz is the standard PCB convention: 1 oz/ft^2 = 35 um.
// ===========================================================================

py::dict trace_width_for_current( double aCurrentA, double aCopperOz, double aTempRiseC,
                                  const std::string& aLayer )
{
    if( !( aCurrentA > 0.0 ) || !std::isfinite( aCurrentA ) )
        throw std::invalid_argument( "trace_width_for_current: current_a must be positive" );
    if( !( aCopperOz > 0.0 ) || !std::isfinite( aCopperOz ) )
        throw std::invalid_argument( "trace_width_for_current: copper_thickness_oz must be positive" );
    if( !( aTempRiseC > 0.0 ) || !std::isfinite( aTempRiseC ) )
        throw std::invalid_argument( "trace_width_for_current: temp_rise_c must be positive" );

    bool internal;
    if( aLayer == "external" || aLayer == "outer" )
        internal = false;
    else if( aLayer == "internal" || aLayer == "inner" )
        internal = true;
    else
        throw std::invalid_argument( "trace_width_for_current: layer must be 'external' or 'internal'" );

    double scale = internal ? 0.024 : 0.048;

    // 1 oz copper = 1.378 mil = 35 um.  pcb_calculator uses 35 um internally.
    double thicknessMil = aCopperOz * 1.378;

    // Inverted IPC formula — solve W from Imax (upstream's TWCalculateWidth):
    //   log W = ( log I - log scale - 0.44 log dT - 0.725 log T ) / 0.725
    double logW = ( std::log( aCurrentA ) - std::log( scale ) - 0.44 * std::log( aTempRiseC )
                    - 0.725 * std::log( thicknessMil ) )
                  / 0.725;
    double widthMil = std::exp( logW );

    double widthMm  = widthMil * 0.0254;       // mil -> mm
    double areaMm2  = widthMm * aCopperOz * 0.035; // 1 oz = 35 um = 0.035 mm

    py::dict out;
    out["ok"]                  = true;
    out["layer"]               = internal ? "internal" : "external";
    out["current_a"]           = aCurrentA;
    out["temp_rise_c"]         = aTempRiseC;
    out["copper_thickness_oz"] = aCopperOz;
    out["width_mil"]           = widthMil;
    out["width_mm"]            = widthMm;
    out["area_mm2"]            = areaMm2;
    return out;
}


// ===========================================================================
// Fusing-current (adiabatic).  Mirrors PANEL_FUSING_CURRENT::m_onCalculateClick
// in pcb_calculator/calculator_panels/panel_fusing_current.cpp.  Copper physical
// constants (specific heat, density, latent heat of fusion, resistivity, tempco)
// are duplicated here verbatim with upstream as the source of truth.
// ===========================================================================

py::dict fusing_current( double aWidthMm, double aThicknessOz, double aFusingTimeS,
                         double aAmbientC, double aMeltC )
{
    if( !( aWidthMm > 0.0 ) || !( aThicknessOz > 0.0 ) || !( aFusingTimeS > 0.0 ) )
        throw std::invalid_argument( "fusing_current: width_mm, thickness_oz, time_s must be positive" );

    // Convert to SI as upstream does (UNIT_MIL etc. — we use mm/oz at the
    // boundary and SI internally).
    double W = aWidthMm * 1.0e-3;            // mm -> m
    double T = aThicknessOz * 35.0e-6;       // oz -> m (1 oz = 35 um)
    double Ta = aAmbientC + 273.15;          // C -> K
    double Tm = aMeltC + 273.15;

    // Constants (mirror panel_fusing_current.cpp).
    const double latentHeat = 205350.0;   // J/kg
    const double cp         = 385.0;      // J/(kg*K)
    const double density    = 8940.0;     // kg/m^3 (copper)

    double deltaEnthalpy = ( Tm - Ta ) * cp;
    double volumicEnergy = density * ( deltaEnthalpy + latentHeat );

    // Resistivity at ambient and at melting (linear tempco model).  ABS_ZERO
    // is implicit in the K conversion above; the panel does it slightly
    // differently (centred on 293 K) — reproduce exactly:
    double Ra = ( ( Ta - 293.0 ) * 0.00393 + 1.0 ) * 1.72e-8;
    double Rm = ( ( Tm - 293.0 ) * 0.00393 + 1.0 ) * 1.72e-8;
    double R  = ( Rm + Ra ) / 2.0;

    double coeff = volumicEnergy / R;
    double A     = W * T;

    double I = A * std::sqrt( coeff / aFusingTimeS );

    py::dict out;
    out["ok"]            = true;
    out["width_mm"]      = aWidthMm;
    out["thickness_oz"]  = aThicknessOz;
    out["ambient_c"]     = aAmbientC;
    out["melt_c"]        = aMeltC;
    out["time_s"]        = aFusingTimeS;
    out["area_m2"]       = A;
    out["current_a"]     = I;
    return out;
}

} // anon


PYBIND11_EMBEDDED_MODULE( klicad_native_pcb_calculator, m )
{
    m.doc() = "KliCAD PCB Calculator binding (pure-math subset).  Reimplements "
              "the formulas from pcb_calculator/* — E-series tables, attenuator "
              "synthesis, IPC-2221 trace width, fusing current — without "
              "instantiating the GUI frame.  Transline (microstrip/stripline/"
              "coplanar/...) and IEC60664 are NOT bound here because the math "
              "lives in symbols not reachable from libkicommon.";

    m.def( "e_series_values", &e_series_values,
           py::arg( "series" ) = std::string( "E24" ),
           R"DOC(Return the canonical [1.0, 10.0) decade slice of the named E-series.

Example:  e_series_values('E12') -> [1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, ...]

Accepts 'E1', 'E3', 'E6', 'E12', 'E24', 'E48', 'E96', 'E192' (case-insensitive,
optional 'E' prefix).  Mirrors ESERIES::ESERIES_VALUES upstream.
)DOC" );

    m.def( "e_series_closest", &e_series_closest,
           py::arg( "target" ),
           py::arg( "series" ) = std::string( "E24" ),
           R"DOC(Snap a target resistor/capacitor value to the nearest E-series value.

Returns {value, error_pct, series, decade}.  `decade` is the power-of-10
offset that places `target` in the [first, first*10) range of the series
base table; useful for tracking decade-boundary picks.
)DOC" );

    m.def( "resistor_combination", &resistor_combination,
           py::arg( "target" ),
           py::arg( "series" ) = std::string( "E24" ),
           R"DOC(Find best 1R, 2R (series or parallel) and 3R combinations of E-series
resistors approximating `target` ohms.

Returns {target, series, single, pair, triple} where each of single/pair/triple
is a dict with {op, value, error_pct, r1, r2[, r3]}.

Mirrors RES_EQUIV_CALC in pcb_calculator/resistor_substitution_utils.cpp.
Limited to E1..E24 (matches upstream — search space on E48+ is impractical).
)DOC" );

    m.def( "attenuator", &attenuator,
           py::arg( "kind" ),
           py::arg( "attenuation_db" ),
           py::arg( "z_in" ),
           py::arg( "z_out" ) = py::none(),
           R"DOC(Synthesise resistor values for a Pi / Tee / Bridged-Tee / Splitter attenuator.

kind: 'pi' | 'tee' | 'bridged_tee' | 'split'
z_out defaults to z_in.  For 'bridged_tee' and 'split' the upstream
implementation forces z_in == z_out.

Returns {ok, kind, atten_db, z_in, z_out, min_atten_db, r1, r2, r3 (if applicable)}.
If the requested attenuation is below `min_atten_db` (impossible for the
given impedance mismatch), `ok` is False and `error` is set.

Mirrors ATTENUATOR_PI/_TEE/_BRIDGE/_SPLITTER ::Calculate() upstream.
)DOC" );

    m.def( "trace_width_for_current", &trace_width_for_current,
           py::arg( "current_a" ),
           py::arg( "copper_thickness_oz" ) = 1.0,
           py::arg( "temp_rise_c" )         = 10.0,
           py::arg( "layer" )               = std::string( "external" ),
           R"DOC(IPC-2221 trace width to carry `current_a` with `temp_rise_c` rise above
ambient, on an `external` or `internal` copper layer of `copper_thickness_oz`
ounces.

Returns {ok, layer, current_a, temp_rise_c, copper_thickness_oz, width_mil,
width_mm, area_mm2}.

Mirrors PANEL_TRACK_WIDTH::TWCalculateWidth in pcb_calculator/calculator_panels/
panel_track_width.cpp.
)DOC" );

    m.def( "fusing_current", &fusing_current,
           py::arg( "width_mm" ),
           py::arg( "thickness_oz" ) = 1.0,
           py::arg( "time_s" )       = 1.0,
           py::arg( "ambient_c" )    = 25.0,
           py::arg( "melt_c" )       = 1083.0,
           R"DOC(Adiabatic fusing current for a copper trace of given geometry, held for
`time_s` at `ambient_c` ambient before fusing.  `melt_c` defaults to copper's
melting point.

Returns {ok, width_mm, thickness_oz, ambient_c, melt_c, time_s, area_m2, current_a}.

Mirrors PANEL_FUSING_CURRENT::m_onCalculateClick in pcb_calculator/
calculator_panels/panel_fusing_current.cpp.
)DOC" );
}
