#include <iostream>
#include <queue>
#include <set>
#include <vector>

#include <stdint.h>
#include <stdio.h>

using namespace std;

class SlidingWindowPercentile {
public:
    SlidingWindowPercentile(int windowSize)
        : windowSize(windowSize) {}

    void addNumber(uint64_t num) {
        if (windowQueue.size() >= windowSize) {
            // Удаляем самый старый элемент
            auto oldest = windowQueue.front();
            windowQueue.pop();
            auto it = windowSorted.find(oldest);
            if (it != windowSorted.end()) {
                windowSorted.erase(it);
            }
        }

        windowQueue.push(num);
        windowSorted.insert(num);
    }

    uint64_t getPercentile(int targetPercentile) {
        if (windowSorted.empty()) {
            return -1;
        }

        int index = (targetPercentile * windowSorted.size()) / 100;
        auto it = windowSorted.begin();
        std::advance(it, index);
        return *it;
    }

private:
    std::queue<uint64_t> windowQueue; 
    std::multiset<uint64_t> windowSorted; 
    int windowSize;
};

int main() {
    SlidingWindowPercentile calculator(1000000); 
    vector<int> p = {50, 80, 90, 99};
    uint64_t num;

    while (fscanf(stdin, "%ld", &num) == 1) {
        calculator.addNumber(num);
        for (auto pp : p) {
            printf("%d: %ld, ", pp, calculator.getPercentile(pp));
        }
        printf("\n");
    }

    return 0;
}

