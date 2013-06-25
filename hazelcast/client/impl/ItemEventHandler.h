//
// Created by sancar koyunlu on 6/24/13.
// Copyright (c) 2013 hazelcast. All rights reserved.




#ifndef HAZELCAST_ITEM_EVENT_HANDLER
#define HAZELCAST_ITEM_EVENT_HANDLER

#include "../serialization/SerializationService.h"
#include "../spi/ClusterService.h"
#include "PortableItemEvent.h"
#include "EntryEvent.h"
#include "ItemEvent.h"

namespace hazelcast {
    namespace client {
        namespace impl {

            template<typename E, typename L>
            class ItemEventHandler {
            public:
                ItemEventHandler(const std::string& instanceName, spi::ClusterService& clusterService, serialization::SerializationService& serializationService, L& listener, bool includeValue)
                :instanceName(instanceName)
                , clusterService(clusterService)
                , serializationService(serializationService)
                , listener(listener)
                , includeValue(includeValue) {

                };

                void handle(const PortableItemEvent& event) {
                    E item;
                    if (includeValue) {
                        item = serializationService.toObject<E>(event.getItem());
                    }
                    connection::Member member = clusterService.getMember(event.getUuid());
                    ItemEvent<E> itemEvent(instanceName, event.getEventType(), item, member);
                    switch (event.getEventType()) {
                        case PortableItemEvent::ItemEventType ::ADDED:
                            listener.entryAdded(itemEvent);
                            break;
                        case PortableItemEvent::ItemEventType::REMOVED:
                            listener.entryRemoved(itemEvent);
                            break;
                        default:
                            break;
                    }

                };
            private:
                const std::string& instanceName;
                serialization::SerializationService& serializationService;
                spi::ClusterService& clusterService;
                L& listener;
                bool includeValue;
            };
        }
    }
}

#endif //HAZELCAST_ITEM_EVENT_HANDLER