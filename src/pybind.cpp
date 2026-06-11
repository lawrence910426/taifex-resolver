#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include <cstring>
#include "parser.h"

namespace py = pybind11;

// Helper to expose a fixed-size char array as a Python str / setter that
// zero-pads or truncates to N bytes.
template <size_t N, typename StructT>
auto fixed_char_getter(char (StructT::*pm)[N]) {
    return [pm](const StructT &p) {
        // `info_time`/`match_time` are filled by snprintf which writes a
        // NUL-terminated string into the 16-byte buffer. `prod_id` is also
        // explicitly NUL-terminated (length 20 + 1 trailing byte).
        return std::string((p.*pm));
    };
}

template <size_t N, typename StructT>
auto fixed_char_setter(char (StructT::*pm)[N]) {
    return [pm](StructT &p, const std::string &value) {
        size_t copy = std::min(value.size(), N - 1);
        std::memcpy(p.*pm, value.data(), copy);
        std::memset((p.*pm) + copy, 0, N - copy);
    };
}

static void bind_header(py::module_ &m) {
    py::class_<Header>(m, "Header")
        .def(py::init<>())
        .def_readwrite("transmission_code", &Header::transmission_code)
        .def_readwrite("message_kind", &Header::message_kind)
        .def_property("info_time",
                      fixed_char_getter<16>(&Header::info_time),
                      fixed_char_setter<16>(&Header::info_time))
        .def_readwrite("channel_id", &Header::channel_id)
        .def_readwrite("channel_seq", &Header::channel_seq)
        .def_readwrite("version_no", &Header::version_no)
        .def_readwrite("body_len", &Header::body_len);
}

static void bind_footer(py::module_ &m) {
    py::class_<Footer>(m, "Footer")
        .def(py::init<>())
        .def_readwrite("checksum", &Footer::checksum)
        .def_readwrite("terminal_code", &Footer::terminal_code);
}

static void bind_match_data(py::module_ &m) {
    py::class_<MatchData>(m, "MatchData")
        .def(py::init<>())
        .def_readwrite("price_sign", &MatchData::price_sign)
        .def_readwrite("price", &MatchData::price)
        .def_readwrite("quantity", &MatchData::quantity)
        .def_readwrite("decimal_locator", &MatchData::decimal_locator);
}

static void bind_md_entry(py::module_ &m) {
    py::class_<MDEntry>(m, "MDEntry")
        .def(py::init<>())
        .def_readwrite("update_action", &MDEntry::update_action)
        .def_readwrite("entry_type", &MDEntry::entry_type)
        .def_readwrite("price_sign", &MDEntry::price_sign)
        .def_readwrite("price", &MDEntry::price)
        .def_readwrite("quantity", &MDEntry::quantity)
        .def_readwrite("price_level", &MDEntry::price_level)
        .def_readwrite("decimal_locator", &MDEntry::decimal_locator);
}

static void bind_i024(py::module_ &m) {
    py::class_<I024_Packet>(m, "I024Packet")
        .def(py::init<>())
        .def_readwrite("header", &I024_Packet::header)
        .def_property("prod_id",
                      fixed_char_getter<21>(&I024_Packet::prod_id),
                      fixed_char_setter<21>(&I024_Packet::prod_id))
        .def_readwrite("prod_msg_seq", &I024_Packet::prod_msg_seq)
        .def_readwrite("calculated_flag", &I024_Packet::calculated_flag)
        .def_property("match_time",
                      fixed_char_getter<16>(&I024_Packet::match_time),
                      fixed_char_setter<16>(&I024_Packet::match_time))
        .def_readwrite("first_price_sign", &I024_Packet::first_price_sign)
        .def_readwrite("first_price", &I024_Packet::first_price)
        .def_readwrite("first_quantity", &I024_Packet::first_quantity)
        .def_readwrite("first_price_decimal", &I024_Packet::first_price_decimal)
        .def_readwrite("display_item", &I024_Packet::display_item)
        .def_readwrite("consecutive_matches", &I024_Packet::consecutive_matches)
        .def_readwrite("total_qty", &I024_Packet::total_qty)
        .def_readwrite("buy_cnt", &I024_Packet::buy_cnt)
        .def_readwrite("sell_cnt", &I024_Packet::sell_cnt)
        .def_readwrite("footer", &I024_Packet::footer);
}

static void bind_i081(py::module_ &m) {
    py::class_<I081_Packet>(m, "I081Packet")
        .def(py::init<>())
        .def_readwrite("header", &I081_Packet::header)
        .def_property("prod_id",
                      fixed_char_getter<21>(&I081_Packet::prod_id),
                      fixed_char_setter<21>(&I081_Packet::prod_id))
        .def_readwrite("prod_msg_seq", &I081_Packet::prod_msg_seq)
        .def_readwrite("no_md_entries", &I081_Packet::no_md_entries)
        .def_readwrite("entries", &I081_Packet::entries)
        .def_readwrite("footer", &I081_Packet::footer);
}

static void bind_i083(py::module_ &m) {
    py::class_<I083_Packet>(m, "I083Packet")
        .def(py::init<>())
        .def_readwrite("header", &I083_Packet::header)
        .def_property("prod_id",
                      fixed_char_getter<21>(&I083_Packet::prod_id),
                      fixed_char_setter<21>(&I083_Packet::prod_id))
        .def_readwrite("prod_msg_seq", &I083_Packet::prod_msg_seq)
        .def_readwrite("calculated_flag", &I083_Packet::calculated_flag)
        .def_readwrite("no_md_entries", &I083_Packet::no_md_entries)
        .def_readwrite("entries", &I083_Packet::entries)
        .def_readwrite("footer", &I083_Packet::footer);
}

PYBIND11_MODULE(taifex_udp_resolver, m) {
    m.doc() = "TAIFEX UDP Resolver (Python interface)";

    bind_header(m);
    bind_footer(m);
    bind_match_data(m);
    bind_md_entry(m);
    bind_i024(m);
    bind_i081(m);
    bind_i083(m);

    py::class_<TaifexParser>(m, "Parser")
        .def(py::init<>())
        // The callbacks must not return anything and accept the corresponding
        // packet types. pybind11/stl.h takes care of std::vector<MDEntry> /
        // std::vector<MatchData> conversions.
        .def("start_loop",
             [](TaifexParser &self,
                int port,
                std::function<void(const I024_Packet &)> cb_i024,
                std::function<void(const I081_Packet &)> cb_i081,
                std::function<void(const I083_Packet &)> cb_i083) {
                 self.start_loop(port, cb_i024, cb_i081, cb_i083);
             },
             py::arg("port"),
             py::arg("cb_i024"),
             py::arg("cb_i081"),
             py::arg("cb_i083"),
             "Start the UDP receive loop. Each callback is invoked from the\n"
             "receive thread; the GIL is acquired automatically by pybind11.")
        .def("end_loop", &TaifexParser::end_loop, "Stop the parsing loop")
        .def("set_multicast", &TaifexParser::set_multicast,
             py::arg("group"), py::arg("iface_ip"),
             "Configure the IPv4 multicast group + local interface IP to join.");
}
