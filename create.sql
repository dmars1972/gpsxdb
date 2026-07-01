-- ============================================================
-- Drop existing tables
-- ============================================================
DROP TABLE IF EXISTS public.nodes;
DROP TABLE IF EXISTS public.ways;
DROP TABLE IF EXISTS public.areas;
DROP TABLE IF EXISTS public.roads;
DROP TABLE IF EXISTS public.relations;
DROP TABLE IF EXISTS public.node_tags;
DROP TABLE IF EXISTS public.way_tags;
DROP TABLE IF EXISTS public.area_tags;
DROP TABLE IF EXISTS public.road_tags;
DROP TABLE IF EXISTS public.relation_tags;

-- ============================================================
-- Create tables
-- ============================================================

CREATE TABLE public.nodes (
    id          bigint NOT NULL,
    name        varchar(256),
    longitude_m double precision,
    latitude_m  double precision,
    geog        public.geometry,
    CONSTRAINT nodes_pkey PRIMARY KEY (id)
);

CREATE TABLE public.ways (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT ways_pkey PRIMARY KEY (id)
);

CREATE TABLE public.areas (
    -- Positive IDs: closed-way areas (id = OSM way id)
    -- Negative IDs: multipolygon/boundary relation areas (id = -OSM relation id)
    -- This convention avoids primary key collisions since OSM way IDs and
    -- relation IDs are separate namespaces that can share the same integers.
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT areas_pkey PRIMARY KEY (id)
);

CREATE TABLE public.roads (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT roads_pkey PRIMARY KEY (id)
);

CREATE TABLE public.relations (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT relations_pkey PRIMARY KEY (id)
);

CREATE TABLE public.node_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.way_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.area_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.road_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.relation_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

-- ============================================================
-- Indexes
-- ============================================================

-- Tag lookup indexes
CREATE INDEX node_tags_idx     ON public.node_tags     (id);
CREATE INDEX way_tags_idx      ON public.way_tags      (id);
CREATE INDEX area_tags_idx     ON public.area_tags     (id);
CREATE INDEX road_tags_idx     ON public.road_tags     (id);
CREATE INDEX relation_tags_idx ON public.relation_tags (id);

-- GiST spatial indexes (built by importer after all data is loaded)
-- CREATE INDEX nodes_geog_idx     ON public.nodes     USING GIST (geog);
-- CREATE INDEX ways_geog_idx      ON public.ways      USING GIST (geog);
-- CREATE INDEX areas_geog_idx     ON public.areas     USING GIST (geog);
-- CREATE INDEX roads_geog_idx     ON public.roads     USING GIST (geog);
-- CREATE INDEX relations_geog_idx ON public.relations USING GIST (geog);

-- Replication state (tracks last applied OSM sequence number)
DROP TABLE IF EXISTS public.osm_replication_state;
CREATE TABLE public.osm_replication_state (
    id       int PRIMARY KEY DEFAULT 1 CHECK (id = 1),  -- singleton row
    sequence bigint NOT NULL
);
