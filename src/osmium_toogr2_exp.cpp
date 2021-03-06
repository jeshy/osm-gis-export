/*

  This is an example tool that converts OSM data to some output format
  like Spatialite or Shapefiles using the OGR library.

  This version does multipolygon handling (in contrast to the osmium_toogr
  example which doesn't).

  This version (..._exp) uses a new experimental unsupported interface.

*/

#include <gdalcpp.hpp>

#include <osmium/experimental/flex_reader.hpp>
#include <osmium/geom/mercator_projection.hpp>
#include <osmium/geom/ogr.hpp>
#include <osmium/handler.hpp>
#include <osmium/index/map/sparse_mem_array.hpp> // IWYU pragma: keep
#include <osmium/io/any_input.hpp> // IWYU pragma: keep
#include <osmium/visitor.hpp>
//#include <osmium/geom/projection.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>

using index_type = osmium::index::map::SparseMemArray<osmium::unsigned_object_id_type, osmium::Location>;
using location_handler_type = osmium::handler::NodeLocationsForWays<index_type>;

template <class TProjection>
class MyOGRHandler : public osmium::handler::Handler {

    gdalcpp::Layer m_layer_point;
    gdalcpp::Layer m_layer_linestring;
    gdalcpp::Layer m_layer_polygon;

    osmium::geom::OGRFactory<TProjection>& m_factory;

public:

    MyOGRHandler(gdalcpp::Dataset& dataset, osmium::geom::OGRFactory<TProjection>& factory) :
        m_layer_point(dataset, "postboxes", wkbPoint),
        m_layer_linestring(dataset, "roads", wkbLineString),
        m_layer_polygon(dataset, "buildings", wkbMultiPolygon),
        m_factory(factory) {

        m_layer_point.add_field("id", OFTReal, 10);
        m_layer_point.add_field("operator", OFTString, 30);

        m_layer_linestring.add_field("id", OFTReal, 10);
        m_layer_linestring.add_field("type", OFTString, 30);

        m_layer_polygon.add_field("id", OFTReal, 10);
        m_layer_polygon.add_field("type", OFTString, 30);
    }

    void node(const osmium::Node& node) {
        const char* amenity = node.tags()["amenity"];
        if (amenity && !std::strcmp(amenity, "post_box")) {
            gdalcpp::Feature feature{m_layer_point, m_factory.create_point(node)};
            feature.set_field("id", static_cast<double>(node.id()));
            feature.set_field("operator", node.tags().get_value_by_key("operator"));
            feature.add_to_layer();
        }
    }

    void way(const osmium::Way& way) {
        const char* highway = way.tags()["highway"];
        if (highway) {
            try {
                gdalcpp::Feature feature{m_layer_linestring, m_factory.create_linestring(way)};
                feature.set_field("id", static_cast<double>(way.id()));
                feature.set_field("type", highway);
                feature.add_to_layer();
            } catch (const osmium::geometry_error&) {
                std::cerr << "Ignoring illegal geometry for way " << way.id() << ".\n";
            }
        }
    }

    void area(const osmium::Area& area) {
        const char* building = area.tags()["building"];
        if (building) {
            try {
                gdalcpp::Feature feature{m_layer_polygon, m_factory.create_multipolygon(area)};
                feature.set_field("id", static_cast<double>(area.id()));
                feature.set_field("type", building);
                feature.add_to_layer();
            } catch (const osmium::geometry_error&) {
                std::cerr << "Ignoring illegal geometry for area "
                          << area.id()
                          << " created from "
                          << (area.from_way() ? "way" : "relation")
                          << " with id="
                          << area.orig_id() << ".\n";
            }
        }
    }

};

/* ================================================== */

void print_help() {
    std::cout << "osmium_toogr2_exp [OPTIONS] [INFILE [OUTFILE]]\n\n" \
              << "If INFILE is not given stdin is assumed.\n" \
              << "If OUTFILE is not given 'ogr_out' is used.\n" \
              << "\nOptions:\n" \
              << "  -h, --help           This help message\n" \
              << "  -f, --format=FORMAT  Output OGR format (Default: 'SQLite')\n";
}

int main(int argc, char* argv[]) {
    try {
        static struct option long_options[] = {
            {"help",   no_argument, nullptr, 'h'},
            {"format", required_argument, nullptr, 'f'},
            {nullptr, 0, nullptr, 0}
        };

        std::string output_format{"SQLite"};

        while (true) {
            const int c = getopt_long(argc, argv, "hf:", long_options, nullptr);
            if (c == -1) {
                break;
            }

            switch (c) {
                case 'h':
                    print_help();
                    return 0;
                case 'f':
                    output_format = optarg;
                    break;
                default:
                    return 1;
            }
        }

        std::string input_filename;
        std::string output_filename{"ogr_out"};
        const int remaining_args = argc - optind;
        if (remaining_args > 2) {
            std::cerr << "Usage: " << argv[0] << " [OPTIONS] [INFILE [OUTFILE]]\n";
            return 1;
        }

        if (remaining_args == 2) {
            input_filename =  argv[optind];
            output_filename = argv[optind+1];
        } else if (remaining_args == 1) {
            input_filename =  argv[optind];
        } else {
            input_filename = "-";
        }

        index_type index;
        location_handler_type location_handler{index};
        osmium::experimental::FlexReader<location_handler_type> exr{input_filename, location_handler, osmium::osm_entity_bits::object};

        // Choose one of the following:

        // 1. Use WGS84, do not project coordinates.
        //osmium::geom::OGRFactory<> factory {};

        // 2. Project coordinates into "Web Mercator".
        osmium::geom::OGRFactory<osmium::geom::MercatorProjection> factory;

        // 3. Use any projection that the proj library can handle.
        //    (Initialize projection with EPSG code or proj string).
        //    In addition you need to link with "-lproj" and add
        //    #include <osmium/geom/projection.hpp>.
        //osmium::geom::OGRFactory<osmium::geom::Projection> factory {osmium::geom::Projection(3857)};

        CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", "OFF");
        gdalcpp::Dataset dataset{output_format, output_filename, gdalcpp::SRS{factory.proj_string()}, { "SPATIALITE=TRUE", "INIT_WITH_EPSG=no" }};
        MyOGRHandler<decltype(factory)::projection_type> ogr_handler{dataset, factory};

        while (auto buffer = exr.read()) {
            osmium::apply(buffer, ogr_handler);
        }

        exr.close();

        std::vector<const osmium::Relation*> incomplete_relations = exr.collector().get_incomplete_relations();
        if (!incomplete_relations.empty()) {
            std::cerr << "Warning! Some member ways missing for these multipolygon relations:";
            for (const auto* relation : incomplete_relations) {
                std::cerr << " " << relation->id();
            }
            std::cerr << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}

