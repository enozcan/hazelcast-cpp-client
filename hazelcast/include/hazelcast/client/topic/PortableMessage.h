//
// Created by sancar koyunlu on 6/25/13.
// Copyright (c) 2013 hazelcast. All rights reserved.



#ifndef HAZELCAST_PortableMessage
#define HAZELCAST_PortableMessage

#include "Portable.h"
#include "Data.h"
#include <string>

namespace hazelcast {
    namespace client {
        namespace topic {
            class PortableMessage : public Portable {
            public:
                PortableMessage();

                PortableMessage(const serialization::Data& message, long publishTime, const std::string& uuid);

                const serialization::Data& getMessage() const;

                std::string getUuid() const;

                long getPublishTime() const;

                int getFactoryId() const;

                int getClassId() const;

                void writePortable(serialization::PortableWriter& writer) const;

                void readPortable(serialization::PortableReader& reader);

            private:
                serialization::Data message;
                std::string uuid;
                long publishTime;
            };
        }
    }
}


#endif //HAZELCAST_PortableMessage
