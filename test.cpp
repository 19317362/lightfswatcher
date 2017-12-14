#include "watch.h"
#include <thread>

int main()
{
    watch::directory dir("dir");
    watch::directory_event event;
 
    while(true)
    {
        while(dir.PollEvent(event))
            std::cout << event.Type << " " << event.Name << std::endl;
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
}
