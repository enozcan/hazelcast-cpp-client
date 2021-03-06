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

#ifndef HAZELCAST_CLIENT_TEST_EXECUTOR_TASKS_SELECTNOMEMBERS_H
#define HAZELCAST_CLIENT_TEST_EXECUTOR_TASKS_SELECTNOMEMBERS_H

#include <hazelcast/client/serialization/IdentifiedDataSerializable.h>
#include <hazelcast/client/cluster/memberselector/MemberSelectors.h>

namespace hazelcast {
    namespace client {
        namespace test {
            namespace executor {
                namespace tasks {
                    class SelectNoMembers
                            : public serialization::IdentifiedDataSerializable,
                              public cluster::memberselector::MemberSelector {
                    public:
                        SelectNoMembers();

                        virtual int getFactoryId() const;

                        virtual int getClassId() const;

                        virtual void writeData(serialization::ObjectDataOutput &writer) const;

                        virtual void readData(serialization::ObjectDataInput &reader);

                        virtual bool select(const Member &member) const;

                        virtual void toString(std::ostream &os) const;
                    };
                }
            }
        }
    }
}


#endif //HAZELCAST_CLIENT_TEST_EXECUTOR_TASKS_SELECTNOMEMBERS_H
