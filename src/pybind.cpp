#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include <cstring>
#include "parser.h"
#include "order_book.h"
#include "taifex_channels.h"

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

static void bind_match_data(py::module_ &m) {
    py::class_<MatchData>(m, "MatchData")
        .def(py::init<>())
        .def_readwrite("price_sign", &MatchData::price_sign)
        .def_readwrite("price", &MatchData::price)
        .def_readwrite("quantity", &MatchData::quantity);
}

static void bind_md_entry(py::module_ &m) {
    py::class_<MDEntry>(m, "MDEntry")
        .def(py::init<>())
        .def_readwrite("update_action", &MDEntry::update_action)
        .def_readwrite("entry_type", &MDEntry::entry_type)
        .def_readwrite("price_sign", &MDEntry::price_sign)
        .def_readwrite("price", &MDEntry::price)
        .def_readwrite("quantity", &MDEntry::quantity)
        .def_readwrite("price_level", &MDEntry::price_level);
}

static void bind_snapshot_entry(py::module_ &m) {
    py::class_<SnapshotEntry>(m, "SnapshotEntry")
        .def(py::init<>())
        .def_readwrite("entry_type", &SnapshotEntry::entry_type)
        .def_readwrite("price_sign", &SnapshotEntry::price_sign)
        .def_readwrite("price", &SnapshotEntry::price)
        .def_readwrite("quantity", &SnapshotEntry::quantity)
        .def_readwrite("price_level", &SnapshotEntry::price_level);
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
        .def_readwrite("display_item", &I024_Packet::display_item)
        .def_readwrite("consecutive_matches", &I024_Packet::consecutive_matches)
        .def_readwrite("total_qty", &I024_Packet::total_qty)
        .def_readwrite("buy_cnt", &I024_Packet::buy_cnt)
        .def_readwrite("sell_cnt", &I024_Packet::sell_cnt);
}

static void bind_i025(py::module_ &m) {
    py::class_<I025_Packet>(m, "I025Packet")
        .def(py::init<>())
        .def_readwrite("header", &I025_Packet::header)
        .def_property("prod_id",
                      fixed_char_getter<21>(&I025_Packet::prod_id),
                      fixed_char_setter<21>(&I025_Packet::prod_id))
        .def_readwrite("prod_msg_seq", &I025_Packet::prod_msg_seq)
        .def_readwrite("day_high_price_sign", &I025_Packet::day_high_price_sign)
        .def_readwrite("day_high_price", &I025_Packet::day_high_price)
        .def_readwrite("day_low_price_sign", &I025_Packet::day_low_price_sign)
        .def_readwrite("day_low_price", &I025_Packet::day_low_price)
        .def_property("show_time",
                      fixed_char_getter<16>(&I025_Packet::show_time),
                      fixed_char_setter<16>(&I025_Packet::show_time));
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
        .def_readwrite("entries", &I081_Packet::entries);
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
        .def_readwrite("entries", &I083_Packet::entries);
}

static void bind_i084_product(py::module_ &m) {
    py::class_<I084Product>(m, "I084Product")
        .def(py::init<>())
        .def_property("prod_id",
                      fixed_char_getter<21>(&I084Product::prod_id),
                      fixed_char_setter<21>(&I084Product::prod_id))
        .def_readwrite("last_prod_msg_seq", &I084Product::last_prod_msg_seq)
        .def_readwrite("no_md_entries", &I084Product::no_md_entries)
        .def_readwrite("entries", &I084Product::entries);
}

static void bind_i084(py::module_ &m) {
    py::class_<I084_Packet>(m, "I084Packet")
        .def(py::init<>())
        .def_readwrite("header", &I084_Packet::header)
        .def_readwrite("message_type", &I084_Packet::message_type)
        .def_readwrite("last_seq", &I084_Packet::last_seq)
        .def_readwrite("no_entries", &I084_Packet::no_entries)
        .def_readwrite("products", &I084_Packet::products);
}

static void bind_order_book_level(py::module_ &m) {
    py::class_<OrderBookLevel>(m, "OrderBookLevel")
        .def(py::init<>())
        .def_readwrite("valid", &OrderBookLevel::valid)
        .def_readwrite("price_sign", &OrderBookLevel::price_sign)
        .def_readwrite("price", &OrderBookLevel::price)
        .def_readwrite("quantity", &OrderBookLevel::quantity);
}

static void bind_order_book(py::module_ &m) {
    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<>())
        .def_property("prod_id",
                      fixed_char_getter<21>(&OrderBook::prod_id),
                      fixed_char_setter<21>(&OrderBook::prod_id))
        .def_readwrite("last_prod_msg_seq", &OrderBook::last_prod_msg_seq)
        .def_readwrite("is_stale", &OrderBook::is_stale)
        .def_readwrite("has_snapshot", &OrderBook::has_snapshot)
        .def_property("info_time",
                      fixed_char_getter<16>(&OrderBook::info_time),
                      fixed_char_setter<16>(&OrderBook::info_time))
        .def_property("snapshot_time",
                      fixed_char_getter<16>(&OrderBook::snapshot_time),
                      fixed_char_setter<16>(&OrderBook::snapshot_time))
        // std::array<OrderBookLevel, 5> converts to a Python list copy.
        .def_readwrite("bids", &OrderBook::bids)
        .def_readwrite("asks", &OrderBook::asks)
        .def_readwrite("derived_bids", &OrderBook::derived_bids)
        .def_readwrite("derived_asks", &OrderBook::derived_asks);
}

static void bind_order_book_manager(py::module_ &m) {
    // Every method releases the GIL around its C++ body: the manager's mutex
    // is also taken by the receive threads, and a Python thread must never
    // block on that mutex while holding the GIL (the receive thread needs the
    // GIL to deliver callbacks).
    using GilRelease = py::call_guard<py::gil_scoped_release>;
    py::class_<OrderBookManager>(m, "OrderBookManager")
        .def(py::init<>())
        // Feed methods, for manual composition / tests. In normal use attach
        // the manager via Parser.set_order_book_manager instead, which also
        // feeds message headers (I001/I002/CHANNEL-SEQ gap tracking).
        .def("on_header", &OrderBookManager::on_header, py::arg("header"),
             GilRelease())
        .def("on_i024", &OrderBookManager::on_i024, py::arg("pkt"), GilRelease())
        .def("on_i025", &OrderBookManager::on_i025, py::arg("pkt"), GilRelease())
        .def("on_i081", &OrderBookManager::on_i081, py::arg("pkt"), GilRelease())
        .def("on_i083", &OrderBookManager::on_i083, py::arg("pkt"), GilRelease())
        .def("on_i084", &OrderBookManager::on_i084, py::arg("pkt"), GilRelease())
        .def("register_callback", &OrderBookManager::register_callback,
             py::arg("prod_id"), py::arg("callback"), GilRelease(),
             "Register a per-instrument book callback, e.g. a\n"
             "handle_futopt_order_book(order_book) function. prod_id is\n"
             "matched with trailing padding trimmed. In C++ the delivered\n"
             "reference is valid only during the call (copy to keep); Python\n"
             "callbacks receive a fresh OrderBook object at the boundary.")
        .def("register_callback_all", &OrderBookManager::register_callback_all,
             py::arg("callback"), GilRelease(),
             "Register a wildcard book callback fired for every instrument.\n"
             "Same delivery contract as register_callback.")
        .def("unregister_callbacks", &OrderBookManager::unregister_callbacks,
             py::arg("prod_id"), GilRelease())
        .def("unregister_all_callbacks",
             &OrderBookManager::unregister_all_callbacks, GilRelease())
        .def("get_book", &OrderBookManager::get_book, py::arg("prod_id"),
             GilRelease(),
             "Copy of the current book for the product, or None if unseen.")
        .def("product_ids", &OrderBookManager::product_ids, GilRelease())
        .def("reset", &OrderBookManager::reset, GilRelease(),
             "Clear all books and sequence trackers (I002-equivalent);\n"
             "delivers a stale-flagged copy of every trusted book first.");
}

namespace {

// Single-channel mode names, matching the MODE vocabulary the parquet
// pipeline's gen_parquet.sh already uses: one capture file == one channel.
struct ModeRow {
    const char* name;
    taifex::constants::Session session;
    taifex::constants::Product product;
    taifex::constants::Service service;
};
constexpr ModeRow kModes[] = {
    {"FUTURES_DAY",            taifex::constants::Session::Day,   taifex::constants::Product::Futures, taifex::constants::Service::Realtime},
    {"FUTURES_NIGHT",          taifex::constants::Session::Night, taifex::constants::Product::Futures, taifex::constants::Service::Realtime},
    {"OPTIONS_DAY",            taifex::constants::Session::Day,   taifex::constants::Product::Options, taifex::constants::Service::Realtime},
    {"OPTIONS_NIGHT",          taifex::constants::Session::Night, taifex::constants::Product::Options, taifex::constants::Service::Realtime},
    {"FUTURES_DAY_SNAPSHOT",   taifex::constants::Session::Day,   taifex::constants::Product::Futures, taifex::constants::Service::SnapshotRefresh},
    {"FUTURES_NIGHT_SNAPSHOT", taifex::constants::Session::Night, taifex::constants::Product::Futures, taifex::constants::Service::SnapshotRefresh},
    {"OPTIONS_DAY_SNAPSHOT",   taifex::constants::Session::Day,   taifex::constants::Product::Options, taifex::constants::Service::SnapshotRefresh},
    {"OPTIONS_NIGHT_SNAPSHOT", taifex::constants::Session::Night, taifex::constants::Product::Options, taifex::constants::Service::SnapshotRefresh},
};

}  // namespace

PYBIND11_MODULE(taifex_udp_resolver, m) {
    m.doc() = "TAIFEX UDP Resolver (Python interface)";

    // The manual's endpoint table, keyed by mode name. No caller should ever
    // type a multicast address by hand.
    {
        py::tuple modes(std::size(kModes));
        for (size_t i = 0; i < std::size(kModes); ++i)
            modes[i] = py::str(kModes[i].name);
        m.attr("MODES") = modes;
    }
    m.def("endpoint",
          [](const std::string& mode) {
              for (const ModeRow& row : kModes) {
                  if (mode == row.name) {
                      const taifex::constants::Channel* ch =
                          taifex::constants::find(row.session, row.product, row.service);
                      return py::make_tuple(std::string(ch->group), ch->port);
                  }
              }
              throw py::value_error("unknown mode '" + mode +
                                    "'; see taifex_udp_resolver.MODES");
          },
          py::arg("mode"),
          "Return (multicast_group, port) for a mode name from MODES,\n"
          "transcribed from the TAIFEX manual (V0.9.5). One mode == one\n"
          "multicast channel.");

    bind_header(m);
    bind_match_data(m);
    bind_md_entry(m);
    bind_snapshot_entry(m);
    bind_i024(m);
    bind_i025(m);
    bind_i081(m);
    bind_i083(m);
    bind_i084_product(m);
    bind_i084(m);
    bind_order_book_level(m);
    bind_order_book(m);
    bind_order_book_manager(m);

    py::class_<TaifexParser>(m, "Parser")
        .def(py::init<>())
        // The callbacks must not return anything and accept the corresponding
        // packet types. pybind11/stl.h takes care of std::vector<MDEntry> /
        // std::vector<MatchData> conversions.
        .def("start_loop",
             [](TaifexParser &self,
                int port,
                std::function<void(const I024_Packet &)> cb_i024,
                std::function<void(const I025_Packet &)> cb_i025,
                std::function<void(const I081_Packet &)> cb_i081,
                std::function<void(const I083_Packet &)> cb_i083,
                std::function<void(const I084_Packet &)> cb_i084) {
                 return self.start_loop(port, cb_i024, cb_i025, cb_i081,
                                        cb_i083, cb_i084);
             },
             py::arg("port"),
             py::arg("cb_i024"),
             py::arg("cb_i025"),
             py::arg("cb_i081"),
             py::arg("cb_i083"),
             py::arg("cb_i084"),
             "Start the UDP receive loop. One callback per message type, in\n"
             "message-ID order; pass None for any type you do not need. Each\n"
             "callback is invoked from the receive thread; the GIL is\n"
             "acquired automatically by pybind11.\n"
             "Returns False when socket setup failed (bad configuration,\n"
             "bind error); the receive thread is not started in that case.")
        // end_loop joins the receive thread, which may be blocked acquiring
        // the GIL to deliver a callback — the GIL must be released while
        // waiting or shutdown deadlocks.
        .def("end_loop", &TaifexParser::end_loop,
             py::call_guard<py::gil_scoped_release>(),
             "Stop the parsing loop")
        .def("set_callbacks",
             [](TaifexParser &self,
                std::function<void(const I024_Packet &)> cb_i024,
                std::function<void(const I025_Packet &)> cb_i025,
                std::function<void(const I081_Packet &)> cb_i081,
                std::function<void(const I083_Packet &)> cb_i083,
                std::function<void(const I084_Packet &)> cb_i084) {
                 self.set_callbacks(cb_i024, cb_i025, cb_i081, cb_i083,
                                    cb_i084);
             },
             py::arg("cb_i024"),
             py::arg("cb_i025"),
             py::arg("cb_i081"),
             py::arg("cb_i083"),
             py::arg("cb_i084"),
             "Install the five per-type callbacks without starting the\n"
             "socket loop (file mode). Pass None for any type you do not\n"
             "need.")
        .def("process_datagram",
             [](TaifexParser &self, py::bytes datagram) {
                 char *buf;
                 py::ssize_t len;
                 if (PYBIND11_BYTES_AS_STRING_AND_SIZE(datagram.ptr(), &buf, &len) != 0)
                     throw py::error_already_set();
                 self.process_datagram(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len));
             },
             py::arg("datagram"),
             "Split one UDP datagram payload on 0x0D 0x0A and feed each\n"
             "message to the parser; callbacks (and the order-book manager,\n"
             "if attached) fire inline on the calling thread.")
        .def("set_multicast", &TaifexParser::set_multicast,
             py::arg("group"), py::arg("iface_ip"),
             "Configure the IPv4 multicast group + local interface IP to join.")
        .def("set_order_book_manager", &TaifexParser::set_order_book_manager,
             py::arg("manager").none(true), py::keep_alive<1, 2>(),
             "Forward decoded I024/I025/I081/I083/I084 packets and every\n"
             "message header to the OrderBookManager (wrapped handle_ mode),\n"
             "before the raw callbacks fire. Call before start_loop; pass\n"
             "None to detach. Attach the same manager to the realtime-port\n"
             "parser and the I084 snapshot-port parser.");
}
