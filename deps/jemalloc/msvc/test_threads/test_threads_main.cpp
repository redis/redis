#include "test_threads.h"
#include <future>
#include <functional>
#include <chrono>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
  int rc = test_threads();
  return rc;
}
