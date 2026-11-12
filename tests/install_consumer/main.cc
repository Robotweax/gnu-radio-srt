// SPDX-License-Identifier: GPL-3.0-or-later
// Uses only installed public headers; no SRT header is reachable from them.
#include <gnuradio/robotweax_srt/m0_probe_sink.h>
#include <gnuradio/robotweax_srt/m0_probe_source.h>
#include <gnuradio/top_block.h>
#include <iostream>

int main()
{
    auto tb = gr::make_top_block("install_consumer");
    auto src = gr::robotweax_srt::m0_probe_source::make(200, true);
    auto snk = gr::robotweax_srt::m0_probe_sink::make(false, "127.0.0.1", 9000);
    tb->connect(src, 0, snk, 0);
    tb->run();
    std::cout << snk->report() << std::endl;
    return 0;
}
