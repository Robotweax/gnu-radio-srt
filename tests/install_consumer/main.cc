// SPDX-License-Identifier: GPL-3.0-or-later
// Uses only installed public headers; no SRT header is reachable from them.
// A Sink and a Source in one process, plaintext on loopback, finite run.
#include <gnuradio/blocks/head.h>
#include <gnuradio/blocks/null_source.h>
#include <gnuradio/blocks/throttle.h>
#include <gnuradio/blocks/vector_sink.h>
#include <gnuradio/robotweax_srt/srt_sink.h>
#include <gnuradio/robotweax_srt/srt_source.h>
#include <gnuradio/top_block.h>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace gr::robotweax_srt;
    transport_config listen;
    listen.role = connection_role::listener;
    listen.port = 9017;
    listen.security = security_mode::none;
    transport_config call = listen;
    call.role = connection_role::caller;
    call.host = "127.0.0.1";
    stream_config stream;

    auto rx = gr::make_top_block("consumer_rx");
    auto source = srt_source::make(listen, stream);
    auto out = gr::blocks::vector_sink_c::make();
    rx->connect(source, 0, out, 0);

    auto tx = gr::make_top_block("consumer_tx");
    auto sink = srt_sink::make(call, stream);
    auto zeros = gr::blocks::null_source::make(sizeof(gr_complex));
    auto pace = gr::blocks::throttle::make(sizeof(gr_complex), 100000, false);
    auto head = gr::blocks::head::make(sizeof(gr_complex), 20000);
    tx->connect(zeros, 0, pace, 0);
    tx->connect(pace, 0, head, 0);
    tx->connect(head, 0, sink, 0);

    rx->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tx->start();
    tx->wait();
    rx->wait();
    std::cout << source->status_json() << std::endl;
    const bool ok = out->data().size() == 20000 &&
                    source->status_json().find("\"end\":\"complete\"") != std::string::npos &&
                    sink->status_json().find("\"completed\":true") != std::string::npos;
    std::cout << (ok ? "consumer transfer complete" : "consumer transfer FAILED") << std::endl;
    return ok ? 0 : 1;
}
