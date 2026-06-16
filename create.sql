-- ============================================================
-- Drop existing tables
-- ============================================================
DROP TABLE IF EXISTS public.my_nodes;
DROP TABLE IF EXISTS public.my_ways;
DROP TABLE IF EXISTS public.my_areas;
DROP TABLE IF EXISTS public.my_roads;
DROP TABLE IF EXISTS public.my_relations;
DROP TABLE IF EXISTS public.my_node_tags;
DROP TABLE IF EXISTS public.my_way_tags;
DROP TABLE IF EXISTS public.my_area_tags;
DROP TABLE IF EXISTS public.my_road_tags;
DROP TABLE IF EXISTS public.my_relation_tags;

-- ============================================================
-- Create tables
-- ============================================================

CREATE TABLE public.my_nodes (
    id          bigint NOT NULL,
    name        varchar(256),
    longitude_m double precision,
    latitude_m  double precision,
    geog        public.geometry,
    CONSTRAINT my_nodes_pkey PRIMARY KEY (id)
);

CREATE TABLE public.my_ways (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT my_ways_pkey PRIMARY KEY (id)
);

CREATE TABLE public.my_areas (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT my_areas_pkey PRIMARY KEY (id)
);

CREATE TABLE public.my_roads (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT my_roads_pkey PRIMARY KEY (id)
);

CREATE TABLE public.my_relations (
    id   bigint NOT NULL,
    name varchar(256),
    geog public.geometry,
    CONSTRAINT my_relations_pkey PRIMARY KEY (id)
);

CREATE TABLE public.my_node_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.my_way_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.my_area_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.my_road_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

CREATE TABLE public.my_relation_tags (
    id        bigint NOT NULL,
    key_name  varchar(256),
    key_value varchar(256)
);

-- ============================================================
-- Indexes
-- ============================================================

-- Tag lookup indexes
CREATE INDEX node_tags_idx     ON public.my_node_tags     (id);
CREATE INDEX way_tags_idx      ON public.my_way_tags      (id);
CREATE INDEX area_tags_idx     ON public.my_area_tags     (id);
CREATE INDEX road_tags_idx     ON public.my_road_tags     (id);
CREATE INDEX relation_tags_idx ON public.my_relation_tags (id);

-- GiST spatial indexes (built by importer after all data is loaded)
-- CREATE INDEX my_nodes_geog_idx     ON public.my_nodes     USING GIST (geog);
-- CREATE INDEX my_ways_geog_idx      ON public.my_ways      USING GIST (geog);
-- CREATE INDEX my_areas_geog_idx     ON public.my_areas     USING GIST (geog);
-- CREATE INDEX my_roads_geog_idx     ON public.my_roads     USING GIST (geog);
-- CREATE INDEX my_relations_geog_idx ON public.my_relations USING GIST (geog);

-- Replication state (tracks last applied OSM sequence number)
DROP TABLE IF EXISTS public.osm_replication_state;
CREATE TABLE public.osm_replication_state (
    id       int PRIMARY KEY DEFAULT 1 CHECK (id = 1),  -- singleton row
    sequence bigint NOT NULL
);
