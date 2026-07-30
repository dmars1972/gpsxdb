#pragma once
#include <string>
#include "DbClient.h"

// Downloads and loads FAA Class/Special Use Airspace and OpenAIP
// international airspace data.
class AirspaceLoader : public DbClient {
public:
    AirspaceLoader(std::string host, std::string user, std::string database)
        : DbClient(std::move(host), std::move(user), std::move(database)) {}

    // Downloads and loads FAA Class Airspace boundaries (Class A/B/C/D/E/G
    // surface areas, Mode-C veils, etc.) into the class_airspace table.
    //
    // Data source: FAA Aeronautical Information Services' public ArcGIS
    // open data portal (adds-faa.opendata.arcgis.com), GeoJSON download,
    // no API key needed. US + territories only (Puerto Rico, Virgin
    // Islands) — matches FAA's own coverage, same as faa_obstacles.
    // Published on an ~8-week cycle alongside the NASR subscription.
    //
    // verbose: when false, suppresses progress output (used by osm_import;
    //          the standalone airspace_load tool passes true).
    //
    // Returns false if the download failed (existing table contents are
    // left untouched in that case).
    bool loadClassAirspace(bool verbose = true);

    // Downloads and loads FAA Special Use Airspace (Military Operations
    // Areas, Restricted, Warning, Alert, and Prohibited areas) into the
    // special_use_airspace table. Same source/coverage/cadence as
    // loadClassAirspace above, separate FAA dataset with a different
    // attribute schema (e.g. times-of-use instead of a CLASS A-G column),
    // hence the separate table.
    bool loadSpecialUseAirspace(bool verbose = true);

    // Downloads and loads global (non-US) airspace from OpenAIP
    // (openaip.net/api.core.openaip.net) into the international_airspace
    // table — crowd-sourced, CC BY-NC 4.0 licensed (noncommercial use
    // only), requires a free API key (see https://www.openaip.net/). US
    // is deliberately excluded: FAA's own class_airspace/special_use_airspace
    // data is the more authoritative source there, same
    // "authoritative-source-stays, other source fills the gap" split as
    // terrain's 3DEP (US) / Copernicus (elsewhere).
    //
    // OpenAIP's numeric `type`/`icao_class`/altitude-unit/altitude-reference
    // codes are stored as-is (raw integers) rather than decoded to names —
    // their meaning is only documented in OpenAIP's JS-rendered API docs
    // (docs.openaip.net), which couldn't be fetched programmatically to
    // build a verified mapping; guessing at these for safety-relevant
    // airspace classification data would be worse than leaving them raw.
    // Consult OpenAIP's docs directly if you need the decode table.
    //
    // api_key: your OpenAIP API key (see openaip.net account settings).
    bool loadInternationalAirspace(const std::string& api_key, bool verbose = true);

    // Downloads and loads FAA Military Training Routes (IR/VR) into the
    // military_training_routes table. Same source/coverage/cadence as
    // loadClassAirspace above. mtr_type (a 0/1 code in the source data,
    // presumably IR vs VR) is stored raw, not decoded -- no coded-value
    // domain on the field and no FAA documentation found confirming which
    // value means which; see the DDL comment in NavDB.cpp for detail.
    bool loadMilitaryTrainingRoutes(bool verbose = true);

    // Downloads and loads the FAA's "National Defense Airspace TFR Areas"
    // layer into the national_defense_tfr table. Deliberately scoped/named:
    // this covers only long-duration security-related TFRs (presidential
    // movements, defense installations) published on FAA's regular ArcGIS
    // data cycle -- NOT the full day-to-day TFR picture pilots see on
    // tfr.faa.gov (stadium games, wildfires, VIP movements change far too
    // fast for a source on this cadence, and the only feed found with that
    // full picture has no geometry at all, just free-text descriptions --
    // not usable for the spatial queries this project is built around).
    bool loadNationalDefenseTFRs(bool verbose = true);

private:
    // (Re)creates public.airspace_at_point() (a UNION ALL across whichever
    // of class_airspace/special_use_airspace/international_airspace/
    // national_defense_tfr currently exist) and
    // public.military_routes_nearby() -- called at the end of each loadXxx()
    // above so both stay correct regardless of which subset actually ran
    // (e.g. no OpenAIP key configured, so international_airspace never gets
    // created). CREATE FUNCTION validates a SQL-language body's table
    // references at creation time, so a table that doesn't exist yet must
    // be left out of the UNION rather than just returning no rows from it.
    void createQueryFunctions();
};

// Reads the OpenAIP API key from ~/.openaip_api_key (never from the repo —
// see loadInternationalAirspace above), or "" if the file doesn't exist.
// Shared by the standalone airspace_load tool and osm_import so the key
// only needs to be saved in one place. Doesn't need a DB connection, so
// stays a free function rather than a method.
std::string defaultOpenAipApiKey();
