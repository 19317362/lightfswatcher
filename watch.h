#pragma once

#include <unordered_map>
#include <vector>
#include <climits>

extern "C"
{
#include "sys/inotify.h"
#include "unistd.h"
}

//#define WATCH_DEBUG 0

#ifdef WATCH_DEBUG
#include <iostream>
#define LOG(...) std::cout << __VA_ARGS__ << std::endl;
#else
#define LOG(...)
#endif


namespace watch
{
    struct directory_event
    {
        enum type
        {
            watch_directory_destroyed, // the watched directory was destroyed
            file_created,
            file_deleted,
            file_modified
        };
    
        type Type;
        std::string Name;
    
        directory_event() :
            Type(watch_directory_destroyed),
            Name({})
        {}
        
        directory_event(type type, const std::string& name) :
                Type(type),
                Name(name)
        {}
    };
    
    template<typename PoolType>
    struct generic_directory_watch
    {
        using id_type = typename PoolType::id_type;
        using pool_type = PoolType;

        std::string Path;
        PoolType* Pool;
        
        id_type NativeHandle = -1;
        int Ticket = -1;
        bool Dead = true;
        
        explicit generic_directory_watch(const std::string& path, PoolType* poolPtr) :
                Path(path),
                Pool(poolPtr)
        {
            Recreate();
        }
        
        void Destroy()
        {
            Pool->Destroy(NativeHandle);
            Dead = true;
        }
        
        void Recreate()
        {
            Destroy();
            
            auto result = Pool->Create(Path.c_str());
            if(result.Error == 0)
            {
                Dead = false;
                NativeHandle = result.Handle;
                Ticket = result.Ticket;
            }
        }
        
        ~generic_directory_watch()
        {
            Destroy();
        }
        
        bool PollEvent(watch::directory_event& event)
        {
            if(Dead)
                Recreate();
            if(Dead) // Recreate should change Dead to false if it succeeded, if it failed we need to bail.
                return false;
            
            Pool->Update();
            auto vec = Pool->GetEvents(NativeHandle);
            if(Ticket >= vec.size())
                return false;
            
            event = vec.at(Ticket++);
            
            if(event.Type == directory_event::watch_directory_destroyed)
                Dead = true;
            
            return true;
        }
    };
    
    template<typename DirectoryWatcherType>
    struct generic_file_watcher
    {
        DirectoryWatcherType DirectoryWatcher;
        std::string Filename;
    
        static std::string GetDirectory(const std::string& dir)
        {
            for(auto iter = dir.rbegin(); iter != dir.rend(); iter++)
            {
                if(*iter == '/' || *iter == '\\')
                    return std::string(dir.begin(), dir.end() - std::distance(dir.rbegin(), iter));
            }
            return {};
        }
    
        static std::string GetFilename(const std::string& dir)
        {
            for(auto iter = dir.rbegin(); iter != dir.rend(); iter++)
            {
                if(*iter == '/' || *iter == '\\')
                    return std::string(dir.end() - std::distance(dir.rbegin(), iter), dir.end());
            }
            return {};
        }
    
        explicit generic_file_watcher(const std::string& dir,
                                      typename DirectoryWatcherType::pool_type* ptr) :
                DirectoryWatcher(GetDirectory(dir), ptr),
                Filename(GetFilename(dir))
        {}
        
        generic_file_watcher(const std::string& dir, const std::string& file) :
                DirectoryWatcher(dir),
                Filename(file)
        { }
        
        bool PollEvent(watch::directory_event& event)
        {
            while(DirectoryWatcher.PollEvent(event))
            {
                if(event.Name == Filename)
                    return true;
            }
            return false;
        }
    };
}

namespace watch_impl
{
    class no_copy
    {
    public:
        no_copy() = default;
        no_copy(no_copy&) = delete;
        no_copy(no_copy&&) = delete;
        no_copy operator=(const no_copy&) = delete;
    };

#ifdef __unix__
    class inotify_watch_pool : public no_copy
    {
    public:
        using id_type = int;

    private:
        int handleInotify_;
        
        
        std::unordered_map<id_type, std::vector<watch::directory_event>> events_ = {};
        
        unsigned char* eventBuffer_ = (unsigned char*)std::calloc(1, 4096);
        
        constexpr static uint32_t DeadFlags = (IN_IGNORED | IN_Q_OVERFLOW | IN_UNMOUNT);
        constexpr static uint32_t FileCreatedFlags = (IN_CREATE | IN_MOVED_TO);
        constexpr static uint32_t FileDeletedFlags= (IN_MOVED_FROM| IN_DELETE);
        constexpr static uint32_t FileModifiedFlags= (IN_MODIFY | IN_CLOSE_WRITE);
        
        uint32_t TranslateToFlags(watch::directory_event::type event)
        {
            using ev = watch::directory_event::type;
            if(event == ev::file_created) return FileCreatedFlags;
            if(event == ev::file_deleted) return FileDeletedFlags;
            if(event == ev::file_modified) return FileModifiedFlags;
            return 0;
        }
        
        
        void ParseEvent(inotify_event& event)
        {
            LOG("Parse " << event.mask);
            
            std::vector<watch::directory_event>& vec = events_[event.wd];
   
            if((event.mask & DeadFlags) != 0)
            {
                // dead
                vec.emplace_back();
            }
            else
            {
                if((event.mask & FileCreatedFlags) != 0)
                    vec.emplace_back(watch::directory_event::file_created, std::string(event.name));
                
                else if((event.mask & FileDeletedFlags) != 0)
                    vec.emplace_back(watch::directory_event::file_deleted, std::string(event.name));
                
                else if((event.mask & FileModifiedFlags) != 0)
                    vec.emplace_back(watch::directory_event::file_modified, std::string(event.name));
                
            }
        }
    
    public:

        inotify_watch_pool() :
                handleInotify_(inotify_init1(IN_NONBLOCK))
        {
        }
        
        ~inotify_watch_pool()
        {
            std::free(eventBuffer_);
        }
    
        struct create_result
        {
            int Error = 0;
            id_type Handle;
            std::vector<watch::directory_event>::size_type Ticket;
        };
        
        create_result Create(const char* file)
        {
            uint32_t flags =  FileCreatedFlags | FileDeletedFlags | FileModifiedFlags;
            
            id_type handle = inotify_add_watch(handleInotify_, file, flags);
            
            return {(handle == -1 ? errno : 0), handle, events_[handle].size()};
        }
        
        void Destroy(id_type id)
        {
            if(id == -1)
                return;
            
            inotify_rm_watch(handleInotify_, id);
        }
        
        void Update()
        {
            constexpr static uint32_t MaxEventSize = sizeof(inotify_event) + NAME_MAX + 1;
            ssize_t len =  read(handleInotify_, eventBuffer_, MaxEventSize);
            ssize_t offset = 0;
            
            if(len == -1)
                return;
      
            while(len > offset)
            {
                inotify_event* ev = (inotify_event*)(offset + eventBuffer_);
                ParseEvent(*ev);
                offset += sizeof(ev->mask) + sizeof(ev->wd) + sizeof(ev->cookie) + sizeof(ev->len) + ev->len;
            }
        }
        
        const std::vector<watch::directory_event>& GetEvents(id_type watch)
        {
            return events_[watch];
        }
    };
#endif
}

namespace watch
{
#if __unix__
    using global_watch_pool_type = watch_impl::inotify_watch_pool;
#endif
    
    using directory = generic_directory_watch<global_watch_pool_type>;
    using file = generic_file_watcher<directory>;
}