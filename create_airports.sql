-- ============================================================
-- Drop existing tables
-- ============================================================
DROP TABLE IF EXISTS public.ap_tags;
DROP TABLE IF EXISTS public.ap_runway_ends;
DROP TABLE IF EXISTS public.ap_runways;
DROP TABLE IF EXISTS public.ap_frequencies;
DROP TABLE IF EXISTS public.ap_navaids;
DROP TABLE IF EXISTS public.ap_airports;
DROP TABLE IF EXISTS public.ap_regions;
DROP TABLE IF EXISTS public.ap_countries;

-- ============================================================
-- Reference tables
-- ============================================================

CREATE TABLE public.ap_countries (
    id          integer PRIMARY KEY,
    code        varchar(2)   NOT NULL,
    name        varchar(256),
    continent   varchar(2)
);

CREATE TABLE public.ap_regions (
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

CREATE TABLE public.ap_airports (
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
-- airport_ident links back to ap_airports.ident
-- entity_type distinguishes the source table
CREATE TABLE public.ap_tags (
    airport_ident   varchar(16)  NOT NULL,
    entity_type     varchar(16)  NOT NULL,  -- 'airport','frequency','runway','navaid'
    key_name        varchar(256) NOT NULL,
    key_value       varchar(256)
);

-- ============================================================
-- Frequencies  (child of airport)
-- ============================================================

CREATE TABLE public.ap_frequencies (
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

CREATE TABLE public.ap_runways (
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

CREATE TABLE public.ap_navaids (
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
CREATE INDEX ap_airports_geog_idx    ON public.ap_airports  USING GIST (geog);
CREATE INDEX ap_airports_ident_idx   ON public.ap_airports  (ident);
CREATE INDEX ap_airports_icao_idx    ON public.ap_airports  (icao_code);
CREATE INDEX ap_airports_iata_idx    ON public.ap_airports  (iata_code);
CREATE INDEX ap_airports_country_idx ON public.ap_airports  (iso_country);
CREATE INDEX ap_airports_region_idx  ON public.ap_airports  (iso_region);
CREATE INDEX ap_airports_type_idx    ON public.ap_airports  (type);



-- Frequencies
CREATE INDEX ap_freq_airport_idx     ON public.ap_frequencies (airport_ref);
CREATE INDEX ap_freq_ident_idx       ON public.ap_frequencies (airport_ident);

-- Runways
CREATE INDEX ap_runways_airport_idx  ON public.ap_runways (airport_ref);
CREATE INDEX ap_runways_le_geog_idx  ON public.ap_runways USING GIST (le_geog);
CREATE INDEX ap_runways_he_geog_idx  ON public.ap_runways USING GIST (he_geog);

-- Navaids
CREATE INDEX ap_navaids_geog_idx     ON public.ap_navaids  USING GIST (geog);
CREATE INDEX ap_navaids_ident_idx    ON public.ap_navaids  (ident);
CREATE INDEX ap_navaids_type_idx     ON public.ap_navaids  (type);
CREATE INDEX ap_navaids_country_idx  ON public.ap_navaids  (iso_country);
CREATE INDEX ap_navaids_airport_idx  ON public.ap_navaids  (associated_airport);

-- Unified tags
CREATE INDEX ap_tags_ident_idx       ON public.ap_tags (airport_ident);
CREATE INDEX ap_tags_type_idx        ON public.ap_tags (airport_ident, entity_type);

-- Regions / Countries
CREATE INDEX ap_regions_country_idx  ON public.ap_regions  (iso_country);
CREATE INDEX ap_regions_code_idx     ON public.ap_regions  (code);
