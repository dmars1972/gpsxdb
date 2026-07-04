-- ============================================================
-- Drop existing tables
-- ============================================================
DROP TABLE IF EXISTS public.tags;
DROP TABLE IF EXISTS public.runways;
DROP TABLE IF EXISTS public.frequencies;
DROP TABLE IF EXISTS public.navaids;
DROP TABLE IF EXISTS public.airports;
DROP TABLE IF EXISTS public.regions;
DROP TABLE IF EXISTS public.countries;

-- ============================================================
-- Reference tables
-- ============================================================

CREATE TABLE public.countries (
    id          integer PRIMARY KEY,
    code        varchar(2)   NOT NULL,
    name        varchar(256),
    continent   varchar(2)
);

CREATE TABLE public.regions (
    id          integer PRIMARY KEY,
    code        varchar(16)  NOT NULL,
    local_code  varchar(16),
    name        varchar(256),
    continent   varchar(2),
    iso_country varchar(2)
);

-- ============================================================
-- Airports
-- ============================================================

CREATE TABLE public.airports (
    id                  integer PRIMARY KEY,
    ident               varchar(16)  NOT NULL,
    type                varchar(32),
    name                varchar(256),
    latitude_m          double precision,
    longitude_m         double precision,
    elevation_ft        integer,
    continent           varchar(2),
    iso_country         varchar(2),
    iso_region          varchar(16),
    municipality        varchar(256),
    scheduled_service   boolean,
    icao_code           varchar(8),
    iata_code           varchar(8),
    gps_code            varchar(8),
    local_code          varchar(16),
    geog                public.geometry
);

-- Unified tag table for all airport-related entities
-- airport_ident links back to airports.ident
-- entity_type distinguishes the source table
CREATE TABLE public.tags (
    airport_ident   varchar(16)  NOT NULL,
    entity_type     varchar(16)  NOT NULL,  -- 'airport','frequency','runway','navaid'
    key_name        varchar(256) NOT NULL,
    key_value       varchar(256)
);

-- ============================================================
-- Frequencies  (child of airport)
-- ============================================================

CREATE TABLE public.frequencies (
    id              integer PRIMARY KEY,
    airport_ref     integer NOT NULL,
    airport_ident   varchar(16),
    type            varchar(16),
    description     varchar(256),
    frequency_mhz   double precision
);

-- ============================================================
-- Runways  (child of airport)
-- ============================================================

CREATE TABLE public.runways (
    id              integer PRIMARY KEY,
    airport_ref     integer NOT NULL,
    airport_ident   varchar(16),
    length_ft       integer,
    width_ft        integer,
    surface         varchar(64),
    lighted         boolean,
    closed          boolean,
    -- Low end
    le_ident                  varchar(8),
    le_latitude_m             double precision,
    le_longitude_m            double precision,
    le_elevation_ft           integer,
    le_heading_degT           double precision,
    le_displaced_threshold_ft integer,
    le_geog                   public.geometry,
    -- High end
    he_ident                  varchar(8),
    he_latitude_m             double precision,
    he_longitude_m            double precision,
    he_elevation_ft           integer,
    he_heading_degT           double precision,
    he_displaced_threshold_ft integer,
    he_geog                   public.geometry
);

-- ============================================================
-- Navaids
-- ============================================================

CREATE TABLE public.navaids (
    id                      integer PRIMARY KEY,
    ident                   varchar(16),
    name                    varchar(256),
    type                    varchar(16),
    frequency_khz           double precision,
    latitude_m              double precision,
    longitude_m             double precision,
    elevation_ft            integer,
    iso_country             varchar(2),
    dme_frequency_khz       double precision,
    dme_channel             varchar(16),
    dme_latitude_m          double precision,
    dme_longitude_m         double precision,
    dme_elevation_ft        integer,
    slaved_variation_deg    double precision,
    magnetic_variation_deg  double precision,
    usage_type              varchar(16),
    power                   varchar(16),
    associated_airport      varchar(16),
    geog                    public.geometry
);


-- ============================================================
-- Indexes
-- ============================================================

-- Airports
CREATE INDEX airports_geog_idx    ON public.airports  USING GIST (geog);
CREATE INDEX airports_ident_idx   ON public.airports  (ident);
CREATE INDEX airports_icao_idx    ON public.airports  (icao_code);
CREATE INDEX airports_iata_idx    ON public.airports  (iata_code);
CREATE INDEX airports_country_idx ON public.airports  (iso_country);
CREATE INDEX airports_region_idx  ON public.airports  (iso_region);
CREATE INDEX airports_type_idx    ON public.airports  (type);



-- Frequencies
CREATE INDEX freq_airport_idx     ON public.frequencies (airport_ref);
CREATE INDEX freq_ident_idx       ON public.frequencies (airport_ident);

-- Runways
CREATE INDEX runways_airport_idx  ON public.runways (airport_ref);
CREATE INDEX runways_le_geog_idx  ON public.runways USING GIST (le_geog);
CREATE INDEX runways_he_geog_idx  ON public.runways USING GIST (he_geog);

-- Navaids
CREATE INDEX navaids_geog_idx     ON public.navaids  USING GIST (geog);
CREATE INDEX navaids_ident_idx    ON public.navaids  (ident);
CREATE INDEX navaids_type_idx     ON public.navaids  (type);
CREATE INDEX navaids_country_idx  ON public.navaids  (iso_country);
CREATE INDEX navaids_airport_idx  ON public.navaids  (associated_airport);

-- Unified tags
CREATE INDEX tags_ident_idx       ON public.tags (airport_ident);
CREATE INDEX tags_type_idx        ON public.tags (airport_ident, entity_type);

-- Regions / Countries
CREATE INDEX regions_country_idx  ON public.regions  (iso_country);
CREATE INDEX regions_code_idx     ON public.regions  (code);

-- FAA Digital Obstacle File (DOF)
-- Updated every 56 days from https://aeronav.faa.gov/Obst_Data/DOF_<date>.zip
-- Coordinates are WGS84. Heights in feet.
DROP TABLE IF EXISTS public.faa_obstacles;
CREATE TABLE public.faa_obstacles (
    id              serial PRIMARY KEY,
    oas_number      varchar(9)  NOT NULL,  -- e.g. "48-123456"
    verified        boolean     NOT NULL,  -- true=verified (O), false=unverified (U)
    country         varchar(2),
    state           varchar(2),
    city            varchar(16),
    latitude        double precision NOT NULL,
    longitude       double precision NOT NULL,
    obstacle_type   varchar(18),
    quantity        integer,
    agl_ht          integer,              -- height above ground level (feet)
    amsl_ht         integer,             -- height above mean sea level (feet)
    lighting        varchar(1),
    horiz_accuracy  varchar(1),
    vert_accuracy   varchar(1),
    marking         varchar(1),
    faa_study_no    varchar(14),
    action          varchar(1),
    julian_date     varchar(7),
    geog            public.geometry      -- point in SRID matching import mode
);

CREATE INDEX faa_obstacles_geog_idx  ON public.faa_obstacles USING GIST (geog);
CREATE INDEX faa_obstacles_type_idx  ON public.faa_obstacles (obstacle_type);
CREATE INDEX faa_obstacles_state_idx ON public.faa_obstacles (state);
CREATE INDEX faa_obstacles_amsl_idx  ON public.faa_obstacles (amsl_ht);
CREATE INDEX faa_obstacles_agl_idx   ON public.faa_obstacles (agl_ht);
