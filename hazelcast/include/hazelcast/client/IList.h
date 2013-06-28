#ifndef HAZELCAST_ILIST
#define HAZELCAST_ILIST

#include "collection/GetAllRequest.h"
#include "collection/ContainsAllRequest.h"
#include "collection/AddAllRequest.h"
#include "collection/CompareAndRemoveRequest.h"
#include "collection/SetRequest.h"
#include "collection/RemoveIndexRequest.h"
#include "collection/IndexOfRequest.h"
#include "collection/AddItemListenerRequest.h"
#include <stdexcept>


namespace hazelcast {
    namespace client {


        template<typename E>
        class IList {
        public:
            IList() {

            };

            void init(const std::string& instanceName, spi::ClientContext *clientContext) {
                context = clientContext;
                key = toData(instanceName);
                proxyId = collection::CollectionProxyId("hz:list:", instanceName, collection::CollectionProxyId::CollectionProxyType::LIST);
            };

            template < typename L>
            long addItemListener(L& listener, bool includeValue) {
                collection::AddItemListenerRequest request(proxyId, includeValue);
                impl::ItemEvent<E> entryEventHandler(proxyId.getName() + proxyId.getKeyName(), context->getClusterService(), context->getSerializationService(), listener, includeValue);
                return context->getServerListenerService().template listen<queue::AddListenerRequest, impl::ItemEventHandler<E, L>, impl::PortableItemEvent >(proxyId.getName() + proxyId.getKeyName(), request, entryEventHandler);
            };

            bool removeItemListener(long registrationId) {
                return context->getServerListenerService().stopListening(proxyId.getName() + proxyId.getKeyName(), registrationId);
            };

            int size() {
                collection::SizeRequest request(proxyId);
                return invoke<int>(request);
            };

            bool isEmpty() {
                return size() == 0;
            };

            bool contains(const E& o) {
                serialization::Data valueData = toData(o);
                collection::ContainsEntryRequest request (proxyId, key, valueData);
                return invoke<bool>(request);
            };

            std::vector<E> toArray() {
                collection::GetAllRequest request(proxyId, key);
                impl::PortableCollection result = invoke<impl::PortableCollection>(request);
                const std::vector<serialization::Data>& collection = result.getCollection();
                std::vector<E> set(collection.size());
                for (int i = 0; i < collection.size(); ++i) {
                    set[i] = toObject<E>(collection[i]);
                }
                return set;
            };

            bool add(const E& e) {
                serialization::Data valueData = toData(e);
                collection::PutRequest request(proxyId, key, valueData, -1, getThreadId());
                return invoke<E>(request);
            };

            bool remove(const E& e) {
                serialization::Data valueData = toData(e);
                collection::RemoveRequest request(proxyId, key, valueData, getThreadId());
                return invoke<bool>(request);
            };

            bool containsAll(const std::vector<E>& objects) {
                collection::ContainsAllRequest request(proxyId, key, toDataCollection(objects));
                return invoke<bool>(request);
            };

            bool addAll(const std::vector<E>& objects) {
                collection::AddAllRequest request(proxyId, key, getThreadId(), toDataCollection(objects));
                return invoke<bool>(request);
            };

            bool addAll(int index, const std::vector<E>& objects) {
                collection::AddAllRequest request(proxyId, key, getThreadId(), toDataCollection(objects), index);
                return invoke<bool>(request);
            };

            bool removeAll(const std::vector<E>& objects) {
                collection::CompareAndRemoveRequest request(proxyId, key, getThreadId(), false, toDataCollection(objects));
                return invoke<bool>(request);
            };

            bool retainAll(const std::vector<E>& objects) {
                collection::CompareAndRemoveRequest request(proxyId, key, getThreadId(), true, toDataCollection(objects));
                return invoke<bool>(request);
            };

            void clear() {
                collection::RemoveAllRequest request(proxyId, key, getThreadId());
                invoke<bool>(request);
            };

            E get(int index) {
                collection::GetRequest request(proxyId, key, index);
                return invoke<E>(request);
            };

            E set(int index, const E& e) {
                serialization::Data valueData = toData(e);
                collection::SetRequest request(proxyId, key, valueData, index, getThreadId());
                return invoke<E>(request);
            };

            void add(int index, const E& e) {
                serialization::Data valueData = toData(e);
                collection::PutRequest request(proxyId, key, valueData, index, getThreadId());
                invoke<bool>(request);
            };

            E remove(int index) {
                collection::RemoveIndexRequest request(proxyId, key, index, getThreadId());
                return invoke<E>(request);
            };

            int indexOf(const E& e) {
                serialization::Data valueData = toData(e);
                collection::IndexOfRequest request(proxyId, key, valueData, false);
                return invoke<int>(request);
            };

            int lastIndexOf(const E& e) {
                serialization::Data valueData = toData(e);
                collection::IndexOfRequest request(proxyId, key, valueData, true);
                return invoke<int>(request);
            };

            std::vector<E> subList(int fromIndex, int toIndex) {
                collection::GetAllRequest request(proxyId, key);
                impl::PortableCollection result = invoke<impl::PortableCollection>(request);
                const std::vector<serialization::Data>& collection = result.getCollection();
                std::vector<E> set(fromIndex - toIndex);
                for (int i = fromIndex; i < collection.size(); ++toIndex) {
                    set[fromIndex - i] = toObject<E>(collection[i]);
                }
                return set;
            };

        private:
            template<typename T>
            std::vector<serialization::Data> toDataCollection(const std::vector<T>& objects) {
                std::vector<serialization::Data> dataCollection(objects.size());
                for (int i = 0; i < objects.size(); ++i) {
                    dataCollection[i] = toData(objects[i]);
                }
                return dataCollection;
            };

            template<typename T>
            serialization::Data toData(const T& object) {
                return context->getSerializationService().toData(object);
            };

            template<typename T>
            T toObject(const serialization::Data& data) {
                return context->getSerializationService().template toObject<T>(data);
            };

            template<typename Response, typename Request>
            Response invoke(const Request& request) {
                return context->getInvocationService().template invokeOnRandomTarget<Response>(request, key);
            };

            int getThreadId() {
                return 1;
            };

            collection::CollectionProxyId proxyId;
            spi::ClientContext *context;
            serialization::Data key;
        };
    }
}

#endif /* HAZELCAST_ILIST */