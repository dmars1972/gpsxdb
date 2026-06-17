psql -d nav -h rpi16 -f create.sql
psql -d nav -h rpi16 -f create_airports.sql
./build/osm_import -i north_america.osm.pbf -s rpi16 -d nav -u daniel \
  -t 3 -w 4 -n 20000000000 -f /nodes/nodes_na_t3.dat -v -l osm_na_t3.log -S /data/nodes -R relations
