#include "Support.h"
#include "AsioHost.h"
#include <iostream>
namespace nce {void PrepareNativeAsioFormat(const WAVEFORMATEX&) {}}
int main(){try{std::cout<<nce::Utf8(nce::AsioStatusJson())<<'\n';return 0;}catch(...){return 1;}}
