/*
 * Copyright (c) 2008-2020, Hazelcast, Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef HAZELCAST_UTIL_CONCURRENT_CONCURRENCYUTIL_H_
#define HAZELCAST_UTIL_CONCURRENT_CONCURRENCYUTIL_H_

#include "hazelcast/util/Executor.h"
#include "hazelcast/util/HazelcastDll.h"

#if  defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#pragma warning(push)
#pragma warning(disable: 4251) //for dll export
#endif

namespace hazelcast {
    namespace util {
        namespace concurrent {
            class HAZELCAST_API ConcurrencyUtil {
            public:
                static const std::shared_ptr<util::Executor> &CALLER_RUNS();
            private:
                class CallerThreadExecutor : public util::Executor {
                public:
                    virtual void execute(const std::shared_ptr<Runnable> &command);
                };

                static const std::shared_ptr<util::Executor> callerRunsExecutor;
            };
        }
    }
}

#if  defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#pragma warning(pop)
#endif

#endif //HAZELCAST_UTIL_CONCURRENT_CONCURRENCYUTIL_H_