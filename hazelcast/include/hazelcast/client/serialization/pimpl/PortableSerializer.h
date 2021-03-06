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
//
//  PortableSerializer.h
//  Server
//
//  Created by sancar koyunlu on 1/10/13.
//  Copyright (c) 2013 sancar koyunlu. All rights reserved.
//

#ifndef HAZELCAST_PORTABLE_SERIALIZER
#define HAZELCAST_PORTABLE_SERIALIZER

#include <memory>

#include "hazelcast/util/HazelcastDll.h"
#include "hazelcast/client/serialization/Serializer.h"

#if  defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#pragma warning(push)
#pragma warning(disable: 4251) //for dll export
#endif

namespace hazelcast {
    namespace client {
        namespace serialization {

            class ClassDefinition;

            class Portable;

            class PortableReader;

            class ObjectDataInput;

            namespace pimpl {

                class PortableContext;

                class HAZELCAST_API PortableSerializer : public StreamSerializer {
                public:

                    PortableSerializer(PortableContext& portableContext);

                    template <typename T>
                    std::unique_ptr<T> readObject(ObjectDataInput &in) {
                        int32_t factoryId = readInt(in);
                        int32_t classId = readInt(in);

                        return read<T>(in, factoryId, classId);
                    }

                    template <typename T>
                    std::unique_ptr<T> read(ObjectDataInput &in, int32_t factoryId, int32_t classId) {
                        std::unique_ptr<Portable> portableInstance = createNewPortableInstance(factoryId, classId);

                        #ifdef __clang__
                        #pragma clang diagnostic push
                        #pragma clang diagnostic ignored "-Wreinterpret-base-class"
                        #endif

                        if (!portableInstance.get()) {
                            portableInstance.reset(reinterpret_cast<Portable *>(new T));
                        }

                        read(in, *portableInstance, factoryId, classId);
                        return std::unique_ptr<T>(reinterpret_cast<T *>(portableInstance.release()));

                        #ifdef __clang__
                        #pragma clang diagnostic pop
                        #endif
                    }

                    virtual int32_t getHazelcastTypeId() const;

                    virtual void write(ObjectDataOutput &out, const void *object);

                    virtual void *read(ObjectDataInput &in);

                    void writeInternal(ObjectDataOutput &out, const Portable *p) const;
                private:
                    PortableContext& context;

                    int findPortableVersion(int factoryId, int classId, const Portable& portable) const;

                    PortableReader createReader(ObjectDataInput& input, int factoryId, int classId, int version, int portableVersion) const;

                    std::unique_ptr<Portable> createNewPortableInstance(int32_t factoryId, int32_t classId);

                    int32_t readInt(ObjectDataInput &in) const;

                    void read(ObjectDataInput &in, Portable &portable, int32_t factoryId, int32_t classId);
                };

            }
        }
    }
}

#if  defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#pragma warning(pop)
#endif

#endif /* HAZELCAST_PORTABLE_SERIALIZER */

