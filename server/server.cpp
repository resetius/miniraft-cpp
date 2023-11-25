#include <poll.hpp>
#include <timesource.h>
#include <raft.h>
#include <server.h>

int main(int argc, char** argv) {
    std::vector<std::string> nodeStrings;
    TNodeDict nodes;
    uint32_t id;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i < argc - 1) {
            // address:port:id
            nodeStrings.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--id") && i < argc - 1) {
            id = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {

        }
    }

    std::shared_ptr<ITimeSource> timeSource = std::make_shared<TTimeSource>();
    NNet::TLoop<NNet::TPoll> loop;

    loop.Loop();
    return 0;
}
