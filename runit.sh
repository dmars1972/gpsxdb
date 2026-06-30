#psql -d nav -h server -f create.sql
#psql -d nav -h server -f create_airports.sql
./build/osm_import -I -i /data/planet-latest.osm.pbf -d nav -s server -u daniel -t 6 -w 12 -n 35000000000 -v -l osm.log -f /data/nodes.dat -S /postgres/tmp
