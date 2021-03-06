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

#include <limits.h>

#include "hazelcast/client/topic/impl/TopicDataSerializerHook.h"
#include "hazelcast/client/impl/ClientLockReferenceIdGenerator.h"
#include "hazelcast/client/crdt/pncounter/impl/PNCounterProxyFactory.h"
#include "hazelcast/client/proxy/ClientPNCounterProxy.h"
#include "hazelcast/client/spi/ClientContext.h"
#include "hazelcast/client/proxy/ClientIdGeneratorProxy.h"
#include "hazelcast/client/idgen/impl/IdGeneratorProxyFactory.h"
#include "hazelcast/client/impl/HazelcastClientInstanceImpl.h"
#include "hazelcast/client/proxy/ClientFlakeIdGeneratorProxy.h"
#include "hazelcast/client/flakeidgen/impl/FlakeIdGeneratorProxyFactory.h"
#include "hazelcast/client/spi/ClientContext.h"
#include "hazelcast/client/proxy/ReliableTopicImpl.h"
#include "hazelcast/client/topic/impl/TopicEventHandlerImpl.h"
#include "hazelcast/client/ClientConfig.h"
#include "hazelcast/client/map/DataEntryView.h"
#include "hazelcast/client/topic/impl/reliable/ReliableTopicExecutor.h"
#include "hazelcast/client/proxy/ClientRingbufferProxy.h"
#include "hazelcast/client/spi/impl/ClientInvocationFuture.h"
#include "hazelcast/client/atomiclong/impl/AtomicLongProxyFactory.h"
#include "hazelcast/client/proxy/ClientAtomicLongProxy.h"
#include "hazelcast/client/ICountDownLatch.h"
#include "hazelcast/client/ILock.h"
#include "hazelcast/client/ISemaphore.h"
#include "hazelcast/client/cluster/impl/VectorClock.h"
#include "hazelcast/client/internal/partition/strategy/StringPartitioningStrategy.h"
#include "hazelcast/util/Util.h"
#include "hazelcast/util/TimeUtil.h"

namespace hazelcast {
    namespace client {
        namespace impl {
            ClientLockReferenceIdGenerator::ClientLockReferenceIdGenerator() : referenceIdCounter(0) {}

            int64_t ClientLockReferenceIdGenerator::getNextReferenceId() {
                return ++referenceIdCounter;
            }
        }

        namespace crdt {
            namespace pncounter {
                namespace impl {
                    PNCounterProxyFactory::PNCounterProxyFactory(spi::ClientContext *clientContext) : clientContext(
                            clientContext) {}

                    std::shared_ptr<spi::ClientProxy> PNCounterProxyFactory::create(const std::string &id) {
                        return std::shared_ptr<spi::ClientProxy>(
                                new proxy::ClientPNCounterProxy(proxy::ClientPNCounterProxy::SERVICE_NAME, id,
                                                                clientContext));
                    }
                }
            }
        }

        namespace idgen {
            namespace impl {
                IdGeneratorProxyFactory::IdGeneratorProxyFactory(spi::ClientContext *clientContext) : clientContext(
                        clientContext) {}

                std::shared_ptr<spi::ClientProxy> IdGeneratorProxyFactory::create(const std::string &id) {
                    IAtomicLong atomicLong = clientContext->getHazelcastClientImplementation()->getIAtomicLong(
                            proxy::ClientIdGeneratorProxy::ATOMIC_LONG_NAME + id);
                    return std::shared_ptr<spi::ClientProxy>(
                            new proxy::ClientIdGeneratorProxy(id, clientContext, atomicLong));
                }
            }
        }

        namespace flakeidgen {
            namespace impl {
                FlakeIdGeneratorProxyFactory::FlakeIdGeneratorProxyFactory(spi::ClientContext *clientContext)
                        : clientContext(
                        clientContext) {}

                std::shared_ptr<spi::ClientProxy> FlakeIdGeneratorProxyFactory::create(const std::string &id) {
                    return std::shared_ptr<spi::ClientProxy>(
                            new proxy::ClientFlakeIdGeneratorProxy(id, clientContext));
                }


                AutoBatcher::AutoBatcher(int32_t batchSize, int64_t validity,
                                         const std::shared_ptr<AutoBatcher::IdBatchSupplier> &batchIdSupplier)
                        : batchSize(batchSize), validity(validity), batchIdSupplier(batchIdSupplier),
                          block(std::shared_ptr<Block>(new Block(IdBatch(0, 0, 0), 0))) {}

                int64_t AutoBatcher::newId() {
                    for (;;) {
                        std::shared_ptr<Block> block = this->block;
                        int64_t res = block->next();
                        if (res != INT64_MIN) {
                            return res;
                        }

                        {
                            std::lock_guard<std::mutex> guard(lock);
                            if (block != this->block.get()) {
                                // new block was assigned in the meantime
                                continue;
                            }
                            this->block = std::shared_ptr<Block>(
                                    new Block(batchIdSupplier->newIdBatch(batchSize), validity));
                        }
                    }
                }

                AutoBatcher::Block::Block(const IdBatch &idBatch, int64_t validity) : idBatch(idBatch), numReturned(0) {
                    invalidSince = validity > 0 ? util::currentTimeMillis() + validity : INT64_MAX;
                }

                int64_t AutoBatcher::Block::next() {
                    if (invalidSince <= util::currentTimeMillis()) {
                        return INT64_MIN;
                    }
                    int32_t index;
                    do {
                        index = numReturned;
                        if (index == idBatch.getBatchSize()) {
                            return INT64_MIN;
                        }
                    } while (!numReturned.compare_exchange_strong(index, index + 1));

                    return idBatch.getBase() + index * idBatch.getIncrement();
                }

                IdBatch::IdIterator IdBatch::endOfBatch;

                const int64_t IdBatch::getBase() const {
                    return base;
                }

                const int64_t IdBatch::getIncrement() const {
                    return increment;
                }

                const int32_t IdBatch::getBatchSize() const {
                    return batchSize;
                }

                IdBatch::IdBatch(const int64_t base, const int64_t increment, const int32_t batchSize)
                        : base(base), increment(increment), batchSize(batchSize) {}

                IdBatch::IdIterator &IdBatch::end() {
                    return endOfBatch;
                }

                IdBatch::IdIterator IdBatch::iterator() {
                    return IdBatch::IdIterator(base, increment, batchSize);
                }

                IdBatch::IdIterator::IdIterator(int64_t base2, const int64_t increment, int32_t remaining) : base2(
                        base2), increment(increment), remaining(remaining) {}

                bool IdBatch::IdIterator::operator==(const IdBatch::IdIterator &rhs) const {
                    return base2 == rhs.base2 && increment == rhs.increment && remaining == rhs.remaining;
                }

                bool IdBatch::IdIterator::operator!=(const IdBatch::IdIterator &rhs) const {
                    return !(rhs == *this);
                }

                IdBatch::IdIterator::IdIterator() : base2(-1), increment(-1), remaining(-1) {
                }

                IdBatch::IdIterator &IdBatch::IdIterator::operator++() {
                    if (remaining == 0) {
                        return IdBatch::end();
                    }

                    --remaining;

                    base2 += increment;

                    return *this;
                }

            }
        }

        namespace proxy {
            MultiMapImpl::MultiMapImpl(const std::string &instanceName, spi::ClientContext *context)
                    : ProxyImpl("hz:impl:multiMapService", instanceName, context) {
                // TODO: remove this line once the client instance getDistributedObject works as expected in Java for this proxy type
                lockReferenceIdGenerator = getContext().getLockReferenceIdGenerator();
            }

            bool MultiMapImpl::put(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapPutCodec::encodeRequest(getName(), key, value,
                                                                         util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MultiMapPutCodec::ResponseParameters>(request, key);
            }

            std::vector<serialization::pimpl::Data> MultiMapImpl::getData(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapGetCodec::encodeRequest(getName(), key,
                                                                         util::getCurrentThreadId());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MultiMapGetCodec::ResponseParameters>(
                        request, key);
            }

            bool MultiMapImpl::remove(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapRemoveEntryCodec::encodeRequest(getName(), key, value,
                                                                                 util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MultiMapRemoveEntryCodec::ResponseParameters>(request,
                                                                                                               key);
            }

            std::vector<serialization::pimpl::Data> MultiMapImpl::removeData(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapRemoveCodec::encodeRequest(getName(), key,
                                                                            util::getCurrentThreadId());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MultiMapRemoveCodec::ResponseParameters>(
                        request, key);
            }

            std::vector<serialization::pimpl::Data> MultiMapImpl::keySetData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapKeySetCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MultiMapKeySetCodec::ResponseParameters>(
                        request);
            }

            std::vector<serialization::pimpl::Data> MultiMapImpl::valuesData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapValuesCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MultiMapValuesCodec::ResponseParameters>(
                        request);
            }

            std::vector<std::pair<serialization::pimpl::Data, serialization::pimpl::Data> >
            MultiMapImpl::entrySetData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapEntrySetCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<std::pair<serialization::pimpl::Data, serialization::pimpl::Data> >, protocol::codec::MultiMapEntrySetCodec::ResponseParameters>(
                        request);
            }

            bool MultiMapImpl::containsKey(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapContainsKeyCodec::encodeRequest(getName(), key,
                                                                                 util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MultiMapContainsKeyCodec::ResponseParameters>(request,
                                                                                                               key);
            }

            bool MultiMapImpl::containsValue(const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapContainsValueCodec::encodeRequest(getName(), value);

                return invokeAndGetResult<bool, protocol::codec::MultiMapContainsValueCodec::ResponseParameters>(
                        request);
            }

            bool MultiMapImpl::containsEntry(const serialization::pimpl::Data &key,
                                             const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapContainsEntryCodec::encodeRequest(getName(), key, value,
                                                                                   util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MultiMapContainsEntryCodec::ResponseParameters>(
                        request, key);
            }

            int MultiMapImpl::size() {
                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MultiMapSizeCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<int, protocol::codec::MultiMapSizeCodec::ResponseParameters>(request);
            }

            void MultiMapImpl::clear() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapClearCodec::encodeRequest(getName());

                invoke(request);
            }

            int MultiMapImpl::valueCount(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapValueCountCodec::encodeRequest(getName(), key,
                                                                                util::getCurrentThreadId());

                return invokeAndGetResult<int, protocol::codec::MultiMapValueCountCodec::ResponseParameters>(request,
                                                                                                             key);
            }

            std::string MultiMapImpl::addEntryListener(impl::BaseEventHandler *entryEventHandler, bool includeValue) {
                return registerListener(createMultiMapEntryListenerCodec(includeValue), entryEventHandler);
            }

            std::string MultiMapImpl::addEntryListener(impl::BaseEventHandler *entryEventHandler,
                                                       serialization::pimpl::Data &key, bool includeValue) {
                return registerListener(createMultiMapEntryListenerCodec(includeValue, key), entryEventHandler);
            }

            bool MultiMapImpl::removeEntryListener(const std::string &registrationId) {
                return getContext().getClientListenerService().deregisterListener(registrationId);
            }

            void MultiMapImpl::lock(const serialization::pimpl::Data &key) {
                lock(key, -1);
            }

            void MultiMapImpl::lock(const serialization::pimpl::Data &key, long leaseTime) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapLockCodec::encodeRequest(getName(), key, util::getCurrentThreadId(),
                                                                          leaseTime,
                                                                          lockReferenceIdGenerator->getNextReferenceId());

                invokeOnPartition(request, partitionId);
            }


            bool MultiMapImpl::isLocked(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapIsLockedCodec::encodeRequest(getName(), key);

                return invokeAndGetResult<bool, protocol::codec::MultiMapIsLockedCodec::ResponseParameters>(request,
                                                                                                            key);
            }

            bool MultiMapImpl::tryLock(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapTryLockCodec::encodeRequest(getName(), key,
                                                                             util::getCurrentThreadId(), LONG_MAX,
                                                                             0,
                                                                             lockReferenceIdGenerator->getNextReferenceId());

                return invokeAndGetResult<bool, protocol::codec::MultiMapTryLockCodec::ResponseParameters>(request,
                                                                                                           key);
            }

            bool MultiMapImpl::tryLock(const serialization::pimpl::Data &key, long timeInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapTryLockCodec::encodeRequest(getName(), key, util::getCurrentThreadId(),
                                                                             LONG_MAX, timeInMillis,
                                                                             lockReferenceIdGenerator->getNextReferenceId());

                return invokeAndGetResult<bool, protocol::codec::MultiMapTryLockCodec::ResponseParameters>(request,
                                                                                                           key);
            }

            void MultiMapImpl::unlock(const serialization::pimpl::Data &key) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapUnlockCodec::encodeRequest(getName(), key, util::getCurrentThreadId(),
                                                                            lockReferenceIdGenerator->getNextReferenceId());

                invokeOnPartition(request, partitionId);
            }

            void MultiMapImpl::forceUnlock(const serialization::pimpl::Data &key) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MultiMapForceUnlockCodec::encodeRequest(getName(), key,
                                                                                 lockReferenceIdGenerator->getNextReferenceId());

                invokeOnPartition(request, partitionId);
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            MultiMapImpl::createMultiMapEntryListenerCodec(bool includeValue) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new MultiMapEntryListenerMessageCodec(getName(), includeValue));
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            MultiMapImpl::createMultiMapEntryListenerCodec(bool includeValue, serialization::pimpl::Data &key) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new MultiMapEntryListenerToKeyCodec(getName(), includeValue, key));
            }

            void MultiMapImpl::onInitialize() {
                ProxyImpl::onInitialize();

                lockReferenceIdGenerator = getContext().getLockReferenceIdGenerator();
            }

            MultiMapImpl::MultiMapEntryListenerMessageCodec::MultiMapEntryListenerMessageCodec(const std::string &name,
                                                                                               bool includeValue)
                    : name(name),
                      includeValue(
                              includeValue) {
            }

            std::unique_ptr<protocol::ClientMessage>
            MultiMapImpl::MultiMapEntryListenerMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::MultiMapAddEntryListenerCodec::encodeRequest(name, includeValue, localOnly);
            }

            std::string MultiMapImpl::MultiMapEntryListenerMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::MultiMapAddEntryListenerCodec::ResponseParameters::decode(
                        responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            MultiMapImpl::MultiMapEntryListenerMessageCodec::encodeRemoveRequest(
                    const std::string &realRegistrationId) const {
                return protocol::codec::MultiMapRemoveEntryListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool MultiMapImpl::MultiMapEntryListenerMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::MultiMapRemoveEntryListenerCodec::ResponseParameters::decode(
                        clientMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            MultiMapImpl::MultiMapEntryListenerToKeyCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::MultiMapAddEntryListenerToKeyCodec::encodeRequest(name, key, includeValue,
                                                                                          localOnly);
            }

            std::string MultiMapImpl::MultiMapEntryListenerToKeyCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::MultiMapAddEntryListenerToKeyCodec::ResponseParameters::decode(
                        responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            MultiMapImpl::MultiMapEntryListenerToKeyCodec::encodeRemoveRequest(
                    const std::string &realRegistrationId) const {
                return protocol::codec::MultiMapRemoveEntryListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool MultiMapImpl::MultiMapEntryListenerToKeyCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::MultiMapRemoveEntryListenerCodec::ResponseParameters::decode(
                        clientMessage).response;
            }

            MultiMapImpl::MultiMapEntryListenerToKeyCodec::MultiMapEntryListenerToKeyCodec(const std::string &name,
                                                                                           bool includeValue,
                                                                                           serialization::pimpl::Data &key)
                    : name(name), includeValue(includeValue), key(key) {}

            ReliableTopicImpl::ReliableTopicImpl(const std::string &instanceName, spi::ClientContext *context,
                                                 std::shared_ptr<Ringbuffer<topic::impl::reliable::ReliableTopicMessage> > rb)
                    : proxy::ProxyImpl("hz:impl:topicService", instanceName, context), ringbuffer(rb),
                      logger(context->getLogger()),
                      config(context->getClientConfig().getReliableTopicConfig(instanceName)) {
            }

            void ReliableTopicImpl::publish(const serialization::pimpl::Data &data) {
                std::unique_ptr<Address> nullAddress;
                topic::impl::reliable::ReliableTopicMessage message(data, nullAddress);
                ringbuffer->add(message);
            }

            const std::string ClientPNCounterProxy::SERVICE_NAME = "hz:impl:PNCounterService";
            const std::shared_ptr<std::set<Address> > ClientPNCounterProxy::EMPTY_ADDRESS_LIST(
                    new std::set<Address>());

            ClientPNCounterProxy::ClientPNCounterProxy(const std::string &serviceName, const std::string &objectName,
                                                       spi::ClientContext *context)
                    : ProxyImpl(serviceName, objectName, context), maxConfiguredReplicaCount(0),
                      observedClock(std::shared_ptr<cluster::impl::VectorClock>(new cluster::impl::VectorClock())),
                      logger(context->getLogger()) {
            }

            std::ostream &operator<<(std::ostream &os, const ClientPNCounterProxy &proxy) {
                os << "PNCounter{name='" << proxy.getName() << "\'}";
                return os;
            }

            int64_t ClientPNCounterProxy::get() {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::get",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeGetInternal(EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);
                protocol::codec::PNCounterGetCodec::ResponseParameters resultParameters = protocol::codec::PNCounterGetCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::getAndAdd(int64_t delta) {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::getAndAdd",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(delta, true, EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::addAndGet(int64_t delta) {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::addAndGet",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(delta, false,
                                                                                      EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::getAndSubtract(int64_t delta) {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::getAndSubtract",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(-delta, true,
                                                                                      EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::subtractAndGet(int64_t delta) {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::subtractAndGet",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(-delta, false,
                                                                                      EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::decrementAndGet() {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::decrementAndGet",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(-1, false, EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::incrementAndGet() {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::incrementAndGet",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(1, false, EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::getAndDecrement() {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::getAndDecrement",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(-1, true, EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            int64_t ClientPNCounterProxy::getAndIncrement() {
                std::shared_ptr<Address> target = getCRDTOperationTarget(*EMPTY_ADDRESS_LIST);
                if (target.get() == NULL) {
                    throw exception::NoDataMemberInClusterException("ClientPNCounterProxy::getAndIncrement",
                                                                    "Cannot invoke operations on a CRDT because the cluster does not contain any data members");
                }
                std::shared_ptr<protocol::ClientMessage> response = invokeAddInternal(1, true, EMPTY_ADDRESS_LIST,
                                                                                      std::unique_ptr<exception::HazelcastException>(),
                                                                                      target);

                protocol::codec::PNCounterAddCodec::ResponseParameters resultParameters = protocol::codec::PNCounterAddCodec::ResponseParameters::decode(
                        *response);
                updateObservedReplicaTimestamps(resultParameters.replicaTimestamps);
                return resultParameters.value;
            }

            void ClientPNCounterProxy::reset() {
                observedClock = std::shared_ptr<cluster::impl::VectorClock>(new cluster::impl::VectorClock());
            }

            std::shared_ptr<Address>
            ClientPNCounterProxy::getCRDTOperationTarget(const std::set<Address> &excludedAddresses) {
                if (currentTargetReplicaAddress.get().get() != NULL &&
                    excludedAddresses.find(*currentTargetReplicaAddress.get()) == excludedAddresses.end()) {
                    return currentTargetReplicaAddress;
                }

                {
                    util::LockGuard guard(targetSelectionMutex);
                    if (currentTargetReplicaAddress.get() == NULL ||
                        excludedAddresses.find(*currentTargetReplicaAddress.get()) != excludedAddresses.end()) {
                        currentTargetReplicaAddress = chooseTargetReplica(excludedAddresses);
                    }
                }
                return currentTargetReplicaAddress;
            }

            std::shared_ptr<Address>
            ClientPNCounterProxy::chooseTargetReplica(const std::set<Address> &excludedAddresses) {
                std::vector<Address> replicaAddresses = getReplicaAddresses(excludedAddresses);
                if (replicaAddresses.empty()) {
                    return std::shared_ptr<Address>();
                }
                // TODO: Use a random generator as used in Java (ThreadLocalRandomProvider) which is per thread
                int randomReplicaIndex = std::abs(rand()) % (int) replicaAddresses.size();
                return std::shared_ptr<Address>(new Address(replicaAddresses[randomReplicaIndex]));
            }

            std::vector<Address> ClientPNCounterProxy::getReplicaAddresses(const std::set<Address> &excludedAddresses) {
                std::vector<Member> dataMembers = getContext().getClientClusterService().getMembers(
                        *cluster::memberselector::MemberSelectors::DATA_MEMBER_SELECTOR);
                int32_t maxConfiguredReplicaCount = getMaxConfiguredReplicaCount();
                int currentReplicaCount = util::min<int>(maxConfiguredReplicaCount, (int) dataMembers.size());

                std::vector<Address> replicaAddresses;
                for (int i = 0; i < currentReplicaCount; i++) {
                    const Address &dataMemberAddress = dataMembers[i].getAddress();
                    if (excludedAddresses.find(dataMemberAddress) == excludedAddresses.end()) {
                        replicaAddresses.push_back(dataMemberAddress);
                    }
                }
                return replicaAddresses;
            }

            int32_t ClientPNCounterProxy::getMaxConfiguredReplicaCount() {
                if (maxConfiguredReplicaCount > 0) {
                    return maxConfiguredReplicaCount;
                } else {
                    std::unique_ptr<protocol::ClientMessage> request = protocol::codec::PNCounterGetConfiguredReplicaCountCodec::encodeRequest(
                            getName());
                    maxConfiguredReplicaCount = invokeAndGetResult<int32_t, protocol::codec::PNCounterGetConfiguredReplicaCountCodec::ResponseParameters>(
                            request);
                }
                return maxConfiguredReplicaCount;
            }

            std::shared_ptr<protocol::ClientMessage>
            ClientPNCounterProxy::invokeGetInternal(std::shared_ptr<std::set<Address> > excludedAddresses,
                                                    const std::unique_ptr<exception::IException> &lastException,
                                                    const std::shared_ptr<Address> &target) {
                if (target.get() == NULL) {
                    if (lastException.get()) {
                        throw *lastException;
                    } else {
                        throw (exception::ExceptionBuilder<exception::NoDataMemberInClusterException>(
                                "ClientPNCounterProxy::invokeGetInternal") <<
                                                                           "Cannot invoke operations on a CRDT because the cluster does not contain any data members").build();
                    }
                }
                try {
                    std::unique_ptr<protocol::ClientMessage> request = protocol::codec::PNCounterGetCodec::encodeRequest(
                            getName(), observedClock.get()->entrySet(), *target);
                    return invokeOnAddress(request, *target);
                } catch (exception::HazelcastException &e) {
                    logger.finest("Exception occurred while invoking operation on target ", *target,
                                  ", choosing different target. Cause: ", e);
                    if (excludedAddresses == EMPTY_ADDRESS_LIST) {
                        // TODO: Make sure that this only affects the local variable of the method
                        excludedAddresses = std::shared_ptr<std::set<Address> >(new std::set<Address>());
                    }
                    excludedAddresses->insert(*target);
                    std::shared_ptr<Address> newTarget = getCRDTOperationTarget(*excludedAddresses);
                    std::unique_ptr<exception::IException> exception = e.clone();
                    return invokeGetInternal(excludedAddresses, exception, newTarget);
                }
            }


            std::shared_ptr<protocol::ClientMessage>
            ClientPNCounterProxy::invokeAddInternal(int64_t delta, bool getBeforeUpdate,
                                                    std::shared_ptr<std::set<Address> > excludedAddresses,
                                                    const std::unique_ptr<exception::IException> &lastException,
                                                    const std::shared_ptr<Address> &target) {
                if (target.get() == NULL) {
                    if (lastException.get()) {
                        throw *lastException;
                    } else {
                        throw (exception::ExceptionBuilder<exception::NoDataMemberInClusterException>(
                                "ClientPNCounterProxy::invokeAddInternal") <<
                                                                           "Cannot invoke operations on a CRDT because the cluster does not contain any data members").build();
                    }
                }

                try {
                    std::unique_ptr<protocol::ClientMessage> request = protocol::codec::PNCounterAddCodec::encodeRequest(
                            getName(), delta, getBeforeUpdate, observedClock.get()->entrySet(), *target);
                    return invokeOnAddress(request, *target);
                } catch (exception::HazelcastException &e) {
                    logger.finest("Unable to provide session guarantees when sending operations to ", *target,
                                  ", choosing different target. Cause: ", e);
                    if (excludedAddresses == EMPTY_ADDRESS_LIST) {
                        // TODO: Make sure that this only affects the local variable of the method
                        excludedAddresses = std::shared_ptr<std::set<Address> >(new std::set<Address>());
                    }
                    excludedAddresses->insert(*target);
                    std::shared_ptr<Address> newTarget = getCRDTOperationTarget(*excludedAddresses);
                    std::unique_ptr<exception::IException> exception = e.clone();
                    return invokeAddInternal(delta, getBeforeUpdate, excludedAddresses, exception, newTarget);
                }
            }

            void ClientPNCounterProxy::updateObservedReplicaTimestamps(
                    const std::vector<std::pair<std::string, int64_t> > &receivedLogicalTimestamps) {
                std::shared_ptr<cluster::impl::VectorClock> received = toVectorClock(receivedLogicalTimestamps);
                for (;;) {
                    std::shared_ptr<cluster::impl::VectorClock> currentClock = this->observedClock;
                    if (currentClock->isAfter(*received)) {
                        break;
                    }
                    if (observedClock.compareAndSet(currentClock, received)) {
                        break;
                    }
                }
            }

            std::shared_ptr<cluster::impl::VectorClock> ClientPNCounterProxy::toVectorClock(
                    const std::vector<std::pair<std::string, int64_t> > &replicaLogicalTimestamps) {
                return std::shared_ptr<cluster::impl::VectorClock>(
                        new cluster::impl::VectorClock(replicaLogicalTimestamps));
            }

            std::shared_ptr<Address> ClientPNCounterProxy::getCurrentTargetReplicaAddress() {
                return currentTargetReplicaAddress.get();
            }

            IListImpl::IListImpl(const std::string &instanceName, spi::ClientContext *context)
                    : ProxyImpl("hz:impl:listService", instanceName, context) {
                serialization::pimpl::Data keyData = getContext().getSerializationService().toData<std::string>(
                        &instanceName);
                partitionId = getPartitionId(keyData);
            }

            std::string IListImpl::addItemListener(impl::BaseEventHandler *entryEventHandler, bool includeValue) {
                return registerListener(createItemListenerCodec(includeValue), entryEventHandler);
            }

            bool IListImpl::removeItemListener(const std::string &registrationId) {
                return getContext().getClientListenerService().deregisterListener(registrationId);
            }

            int IListImpl::size() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListSizeCodec::encodeRequest(getName());

                return invokeAndGetResult<int, protocol::codec::ListSizeCodec::ResponseParameters>(request,
                                                                                                   partitionId);
            }

            bool IListImpl::isEmpty() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListIsEmptyCodec::encodeRequest(getName());

                return invokeAndGetResult<bool, protocol::codec::ListIsEmptyCodec::ResponseParameters>(request,
                                                                                                       partitionId);
            }

            bool IListImpl::contains(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListContainsCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::ListContainsCodec::ResponseParameters>(request,
                                                                                                        partitionId);
            }

            std::vector<serialization::pimpl::Data> IListImpl::toArrayData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListGetAllCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::ListGetAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool IListImpl::add(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListAddCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::ListAddCodec::ResponseParameters>(request,
                                                                                                   partitionId);
            }

            bool IListImpl::remove(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListRemoveCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::ListRemoveCodec::ResponseParameters>(request,
                                                                                                      partitionId);
            }

            bool IListImpl::containsAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListContainsAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::ListContainsAllCodec::ResponseParameters>(request,
                                                                                                           partitionId);
            }

            bool IListImpl::addAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListAddAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::ListAddAllCodec::ResponseParameters>(request,
                                                                                                      partitionId);
            }

            bool IListImpl::addAll(int index, const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListAddAllWithIndexCodec::encodeRequest(getName(), index,
                                                                                 elements);

                return invokeAndGetResult<bool, protocol::codec::ListAddAllWithIndexCodec::ResponseParameters>(request,
                                                                                                               partitionId);
            }

            bool IListImpl::removeAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListCompareAndRemoveAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::ListCompareAndRemoveAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool IListImpl::retainAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListCompareAndRetainAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::ListCompareAndRetainAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            void IListImpl::clear() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListClearCodec::encodeRequest(getName());

                invokeOnPartition(request, partitionId);
            }

            std::unique_ptr<serialization::pimpl::Data> IListImpl::getData(int index) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListGetCodec::encodeRequest(getName(), index);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::ListGetCodec::ResponseParameters>(
                        request, partitionId);
            }

            std::unique_ptr<serialization::pimpl::Data> IListImpl::setData(int index,
                                                                           const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListSetCodec::encodeRequest(getName(), index, element);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::ListSetCodec::ResponseParameters>(
                        request, partitionId);
            }

            void IListImpl::add(int index, const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListAddWithIndexCodec::encodeRequest(getName(), index, element);

                invokeOnPartition(request, partitionId);
            }

            std::unique_ptr<serialization::pimpl::Data> IListImpl::removeData(int index) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListRemoveWithIndexCodec::encodeRequest(getName(), index);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::ListRemoveWithIndexCodec::ResponseParameters>(
                        request, partitionId);
            }

            int IListImpl::indexOf(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListIndexOfCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<int, protocol::codec::ListIndexOfCodec::ResponseParameters>(request,
                                                                                                      partitionId);
            }

            int IListImpl::lastIndexOf(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListLastIndexOfCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<int, protocol::codec::ListLastIndexOfCodec::ResponseParameters>(request,
                                                                                                          partitionId);
            }

            std::vector<serialization::pimpl::Data> IListImpl::subListData(int fromIndex, int toIndex) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::ListSubCodec::encodeRequest(getName(), fromIndex, toIndex);

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::ListSubCodec::ResponseParameters>(
                        request, partitionId);
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            IListImpl::createItemListenerCodec(bool includeValue) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new ListListenerMessageCodec(getName(), includeValue));
            }

            IListImpl::ListListenerMessageCodec::ListListenerMessageCodec(const std::string &name,
                                                                          bool includeValue) : name(name),
                                                                                               includeValue(
                                                                                                       includeValue) {}

            std::unique_ptr<protocol::ClientMessage>
            IListImpl::ListListenerMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::ListAddListenerCodec::encodeRequest(name, includeValue, localOnly);
            }

            std::string IListImpl::ListListenerMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::ListAddListenerCodec::ResponseParameters::decode(responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            IListImpl::ListListenerMessageCodec::encodeRemoveRequest(const std::string &realRegistrationId) const {
                return protocol::codec::ListRemoveListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool IListImpl::ListListenerMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::ListRemoveListenerCodec::ResponseParameters::decode(clientMessage).response;
            }

            const std::string ClientIdGeneratorProxy::SERVICE_NAME = "hz:impl:idGeneratorService";
            const std::string ClientIdGeneratorProxy::ATOMIC_LONG_NAME = "hz:atomic:idGenerator:";

            ClientIdGeneratorProxy::ClientIdGeneratorProxy(const std::string &instanceName, spi::ClientContext *context,
                                                           const IAtomicLong &atomicLong)
                    : proxy::ProxyImpl(ClientIdGeneratorProxy::SERVICE_NAME, instanceName, context),
                      atomicLong(atomicLong), local(new std::atomic<int64_t>(-1)),
                      residue(new std::atomic<int32_t>(BLOCK_SIZE)), localLock(new util::Mutex) {
                this->atomicLong.get();
            }

            bool ClientIdGeneratorProxy::init(int64_t id) {
                if (id < 0) {
                    return false;
                }
                int64_t step = (id / BLOCK_SIZE);

                util::LockGuard lg(*localLock);
                bool init = atomicLong.compareAndSet(0, step + 1);
                if (init) {
                    local->store(step);
                    residue->store((int32_t) (id % BLOCK_SIZE) + 1);
                }
                return init;
            }

            int64_t ClientIdGeneratorProxy::newId() {
                int64_t block = local->load();
                int32_t value = (*residue)++;

                if (local->load() != block) {
                    return newId();
                }

                if (value < BLOCK_SIZE) {
                    return block * BLOCK_SIZE + value;
                }

                {
                    util::LockGuard lg(*localLock);
                    value = *residue;
                    if (value >= BLOCK_SIZE) {
                        *local = atomicLong.getAndIncrement();
                        *residue = 0;
                    }
                }

                return newId();
            }

            void ClientIdGeneratorProxy::destroy() {
                util::LockGuard lg(*localLock);
                atomicLong.destroy();
                *local = -1;
                *residue = BLOCK_SIZE;
            }

            const std::string ClientFlakeIdGeneratorProxy::SERVICE_NAME = "hz:impl:flakeIdGeneratorService";

            ClientFlakeIdGeneratorProxy::ClientFlakeIdGeneratorProxy(const std::string &objectName,
                                                                     spi::ClientContext *context)
                    : ProxyImpl(SERVICE_NAME, objectName, context) {
                std::shared_ptr<config::ClientFlakeIdGeneratorConfig> config = getContext().getClientConfig().findFlakeIdGeneratorConfig(
                        getName());
                batcher.reset(
                        new flakeidgen::impl::AutoBatcher(config->getPrefetchCount(),
                                                          config->getPrefetchValidityMillis(),
                                                          std::shared_ptr<flakeidgen::impl::AutoBatcher::IdBatchSupplier>(
                                                                  new FlakeIdBatchSupplier(*this))));
            }

            int64_t ClientFlakeIdGeneratorProxy::newId() {
                return batcher->newId();
            }

            flakeidgen::impl::IdBatch ClientFlakeIdGeneratorProxy::newIdBatch(int32_t batchSize) {
                std::unique_ptr<protocol::ClientMessage> requestMsg = protocol::codec::FlakeIdGeneratorNewIdBatchCodec::encodeRequest(
                        getName(), batchSize);
                std::shared_ptr<protocol::ClientMessage> responseMsg = spi::impl::ClientInvocation::create(
                        getContext(), requestMsg, getName())->invoke()->get();
                protocol::codec::FlakeIdGeneratorNewIdBatchCodec::ResponseParameters response =
                        protocol::codec::FlakeIdGeneratorNewIdBatchCodec::ResponseParameters::decode(*responseMsg);
                return flakeidgen::impl::IdBatch(response.base, response.increment, response.batchSize);
            }

            bool ClientFlakeIdGeneratorProxy::init(int64_t id) {
                // Add 1 hour worth of IDs as a reserve: due to long batch validity some clients might be still getting
                // older IDs. 1 hour is just a safe enough value, not a real guarantee: some clients might have longer
                // validity.
                // The init method should normally be called before any client generated IDs: in this case no reserve is
                // needed, so we don't want to increase the reserve excessively.
                int64_t reserve =
                        (int64_t) (3600 * 1000) /* 1 HOUR in milliseconds */ << (BITS_NODE_ID + BITS_SEQUENCE);
                return newId() >= id + reserve;
            }

            ClientFlakeIdGeneratorProxy::FlakeIdBatchSupplier::FlakeIdBatchSupplier(ClientFlakeIdGeneratorProxy &proxy)
                    : proxy(proxy) {}

            flakeidgen::impl::IdBatch ClientFlakeIdGeneratorProxy::FlakeIdBatchSupplier::newIdBatch(int32_t batchSize) {
                return proxy.newIdBatch(batchSize);
            }

            IQueueImpl::IQueueImpl(const std::string &instanceName, spi::ClientContext *context)
                    : ProxyImpl("hz:impl:queueService", instanceName, context) {
                serialization::pimpl::Data data = getContext().getSerializationService().toData<std::string>(
                        &instanceName);
                partitionId = getPartitionId(data);
            }

            std::string IQueueImpl::addItemListener(impl::BaseEventHandler *itemEventHandler, bool includeValue) {
                return registerListener(createItemListenerCodec(includeValue), itemEventHandler);
            }

            bool IQueueImpl::removeItemListener(const std::string &registrationId) {
                return getContext().getClientListenerService().deregisterListener(registrationId);
            }

            bool IQueueImpl::offer(const serialization::pimpl::Data &element, long timeoutInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueOfferCodec::encodeRequest(getName(), element,
                                                                        timeoutInMillis);

                return invokeAndGetResult<bool, protocol::codec::QueueOfferCodec::ResponseParameters>(request,
                                                                                                      partitionId);
            }

            void IQueueImpl::put(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueuePutCodec::encodeRequest(getName(), element);

                invokeOnPartition(request, partitionId);
            }

            std::unique_ptr<serialization::pimpl::Data> IQueueImpl::pollData(long timeoutInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueuePollCodec::encodeRequest(getName(), timeoutInMillis);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::QueuePollCodec::ResponseParameters>(
                        request, partitionId);
            }

            int IQueueImpl::remainingCapacity() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueRemainingCapacityCodec::encodeRequest(getName());

                return invokeAndGetResult<int, protocol::codec::QueueRemainingCapacityCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool IQueueImpl::remove(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueRemoveCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::QueueRemoveCodec::ResponseParameters>(request,
                                                                                                       partitionId);
            }

            bool IQueueImpl::contains(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueContainsCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::QueueContainsCodec::ResponseParameters>(request,
                                                                                                         partitionId);
            }

            std::vector<serialization::pimpl::Data> IQueueImpl::drainToData(size_t maxElements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueDrainToMaxSizeCodec::encodeRequest(getName(), (int32_t) maxElements);

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::QueueDrainToMaxSizeCodec::ResponseParameters>(
                        request, partitionId);
            }

            std::vector<serialization::pimpl::Data> IQueueImpl::drainToData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueDrainToCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::QueueDrainToMaxSizeCodec::ResponseParameters>(
                        request, partitionId);
            }

            std::unique_ptr<serialization::pimpl::Data> IQueueImpl::peekData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueuePeekCodec::encodeRequest(getName());

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::QueuePeekCodec::ResponseParameters>(
                        request, partitionId);
            }

            int IQueueImpl::size() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueSizeCodec::encodeRequest(getName());

                return invokeAndGetResult<int, protocol::codec::QueueSizeCodec::ResponseParameters>(request,
                                                                                                    partitionId);
            }

            bool IQueueImpl::isEmpty() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueIsEmptyCodec::encodeRequest(getName());

                return invokeAndGetResult<bool, protocol::codec::QueueIsEmptyCodec::ResponseParameters>(request,
                                                                                                        partitionId);
            }

            std::vector<serialization::pimpl::Data> IQueueImpl::toArrayData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueIteratorCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::QueueIteratorCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool IQueueImpl::containsAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueContainsAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::QueueContainsAllCodec::ResponseParameters>(request,
                                                                                                            partitionId);
            }

            bool IQueueImpl::addAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueAddAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::QueueAddAllCodec::ResponseParameters>(request,
                                                                                                       partitionId);
            }

            bool IQueueImpl::removeAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueCompareAndRemoveAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::QueueCompareAndRemoveAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool IQueueImpl::retainAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueCompareAndRetainAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::QueueCompareAndRetainAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            void IQueueImpl::clear() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::QueueClearCodec::encodeRequest(getName());

                invokeOnPartition(request, partitionId);
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            IQueueImpl::createItemListenerCodec(bool includeValue) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new QueueListenerMessageCodec(getName(), includeValue));
            }

            IQueueImpl::QueueListenerMessageCodec::QueueListenerMessageCodec(const std::string &name,
                                                                             bool includeValue) : name(name),
                                                                                                  includeValue(
                                                                                                          includeValue) {}

            std::unique_ptr<protocol::ClientMessage>
            IQueueImpl::QueueListenerMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::QueueAddListenerCodec::encodeRequest(name, includeValue, localOnly);
            }

            std::string IQueueImpl::QueueListenerMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::QueueAddListenerCodec::ResponseParameters::decode(responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            IQueueImpl::QueueListenerMessageCodec::encodeRemoveRequest(const std::string &realRegistrationId) const {
                return protocol::codec::QueueRemoveListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool IQueueImpl::QueueListenerMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::QueueRemoveListenerCodec::ResponseParameters::decode(clientMessage).response;
            }

            ProxyImpl::ProxyImpl(const std::string &serviceName, const std::string &objectName,
                                 spi::ClientContext *context)
                    : ClientProxy(objectName, serviceName, *context) {
            }

            ProxyImpl::~ProxyImpl() {
            }

            int ProxyImpl::getPartitionId(const serialization::pimpl::Data &key) {
                return getContext().getPartitionService().getPartitionId(key);
            }

            std::shared_ptr<protocol::ClientMessage> ProxyImpl::invokeOnPartition(
                    std::unique_ptr<protocol::ClientMessage> &request, int partitionId) {
                try {
                    std::shared_ptr<spi::impl::ClientInvocationFuture> future = invokeAndGetFuture(request,
                                                                                                   partitionId);
                    return future->get();
                } catch (exception::IException &e) {
                    util::ExceptionUtil::rethrow(e);
                }
                return std::shared_ptr<protocol::ClientMessage>();
            }

            std::shared_ptr<spi::impl::ClientInvocationFuture>
            ProxyImpl::invokeAndGetFuture(std::unique_ptr<protocol::ClientMessage> &request, int partitionId) {
                try {
                    std::shared_ptr<spi::impl::ClientInvocation> invocation = spi::impl::ClientInvocation::create(
                            getContext(), request, getName(), partitionId);
                    return invocation->invoke();
                } catch (exception::IException &e) {
                    util::ExceptionUtil::rethrow(e);
                }
                return std::shared_ptr<spi::impl::ClientInvocationFuture>();
            }

            std::shared_ptr<spi::impl::ClientInvocationFuture>
            ProxyImpl::invokeOnKeyOwner(std::unique_ptr<protocol::ClientMessage> &request,
                                        const serialization::pimpl::Data &keyData) {
                int partitionId = getPartitionId(keyData);
                std::shared_ptr<spi::impl::ClientInvocation> invocation = spi::impl::ClientInvocation::create(
                        getContext(), request, getName(), partitionId);
                return invocation->invoke();
            }

            std::shared_ptr<protocol::ClientMessage>
            ProxyImpl::invoke(std::unique_ptr<protocol::ClientMessage> &request) {
                try {
                    std::shared_ptr<spi::impl::ClientInvocation> invocation = spi::impl::ClientInvocation::create(
                            getContext(), request, getName());
                    return invocation->invoke()->get();
                } catch (exception::IException &e) {
                    util::ExceptionUtil::rethrow(e);
                }
                return std::shared_ptr<protocol::ClientMessage>();
            }

            std::shared_ptr<protocol::ClientMessage>
            ProxyImpl::invokeOnAddress(std::unique_ptr<protocol::ClientMessage> &request, const Address &address) {
                try {

                    std::shared_ptr<spi::impl::ClientInvocation> invocation = spi::impl::ClientInvocation::create(
                            getContext(), request, getName(), address);
                    return invocation->invoke()->get();
                } catch (exception::IException &e) {
                    util::ExceptionUtil::rethrow(e);
                }
                return std::shared_ptr<protocol::ClientMessage>();
            }

            std::vector<hazelcast::client::TypedData>
            ProxyImpl::toTypedDataCollection(const std::vector<serialization::pimpl::Data> &values) {
                std::vector<hazelcast::client::TypedData> result;
                typedef std::vector<serialization::pimpl::Data> VALUES;
                for (const VALUES::value_type &value : values) {
                    result.push_back(TypedData(
                            std::unique_ptr<serialization::pimpl::Data>(
                                    new serialization::pimpl::Data(value)),
                            getContext().getSerializationService()));
                }
                return result;
            }

            std::vector<std::pair<TypedData, TypedData> > ProxyImpl::toTypedDataEntrySet(
                    const std::vector<std::pair<serialization::pimpl::Data, serialization::pimpl::Data> > &dataEntrySet) {
                typedef std::vector<std::pair<serialization::pimpl::Data, serialization::pimpl::Data> > ENTRIES_DATA;
                std::vector<std::pair<TypedData, TypedData> > result;
                for (const ENTRIES_DATA::value_type &value : dataEntrySet) {
                    serialization::pimpl::SerializationService &serializationService = getContext().getSerializationService();
                    TypedData keyData(std::unique_ptr<serialization::pimpl::Data>(
                            new serialization::pimpl::Data(value.first)), serializationService);
                    TypedData valueData(std::unique_ptr<serialization::pimpl::Data>(
                            new serialization::pimpl::Data(value.second)), serializationService);
                    result.push_back(std::make_pair(keyData, valueData));
                }
                return result;
            }

            std::shared_ptr<serialization::pimpl::Data> ProxyImpl::toShared(const serialization::pimpl::Data &data) {
                return std::shared_ptr<serialization::pimpl::Data>(new serialization::pimpl::Data(data));
            }

            PartitionSpecificClientProxy::PartitionSpecificClientProxy(const std::string &serviceName,
                                                                       const std::string &objectName,
                                                                       spi::ClientContext *context) : ProxyImpl(
                    serviceName, objectName, context) {}

            void PartitionSpecificClientProxy::onInitialize() {
                std::string partitionKey = internal::partition::strategy::StringPartitioningStrategy::getPartitionKey(
                        name);
                partitionId = getContext().getPartitionService().getPartitionId(toData<std::string>(partitionKey));
            }

            const std::string ClientAtomicLongProxy::SERVICE_NAME = "hz:impl:atomicLongService";

            ClientAtomicLongProxy::ClientAtomicLongProxy(const std::string &objectName, spi::ClientContext *context)
                    : PartitionSpecificClientProxy(SERVICE_NAME, objectName, context) {
            }

            int64_t ClientAtomicLongProxy::addAndGet(int64_t delta) {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(
                        addAndGetAsync(delta))->join());
            }

            bool ClientAtomicLongProxy::compareAndSet(int64_t expect, int64_t update) {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<bool> >(
                        compareAndSetAsync(expect, update))->join());
            }

            int64_t ClientAtomicLongProxy::decrementAndGet() {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(
                        decrementAndGetAsync())->join());
            }

            int64_t ClientAtomicLongProxy::get() {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(getAsync())->join());
            }

            int64_t ClientAtomicLongProxy::getAndAdd(int64_t delta) {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(
                        getAndAddAsync(delta))->join());
            }

            int64_t ClientAtomicLongProxy::getAndSet(int64_t newValue) {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(
                        getAndSetAsync(newValue))->join());
            }

            int64_t ClientAtomicLongProxy::incrementAndGet() {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(
                        incrementAndGetAsync())->join());
            }

            int64_t ClientAtomicLongProxy::getAndIncrement() {
                return *(std::static_pointer_cast<spi::InternalCompletableFuture<int64_t> >(
                        getAndIncrementAsync())->join());
            }

            void ClientAtomicLongProxy::set(int64_t newValue) {
                std::static_pointer_cast<spi::InternalCompletableFuture<void> >(setAsync(newValue))->join();
            }

            std::shared_ptr<ICompletableFuture<int64_t> >
            ClientAtomicLongProxy::addAndGetAsync(int64_t delta) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongAddAndGetCodec::encodeRequest(name, delta);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongAddAndGetCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<bool> >
            ClientAtomicLongProxy::compareAndSetAsync(int64_t expect, int64_t update) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongCompareAndSetCodec::encodeRequest(name, expect, update);

                return invokeOnPartitionAsync<bool>(request,
                                                    impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongCompareAndSetCodec, bool>::instance());
            }

            std::shared_ptr<ICompletableFuture<int64_t> > ClientAtomicLongProxy::decrementAndGetAsync() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongDecrementAndGetCodec::encodeRequest(name);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongDecrementAndGetCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<int64_t> > ClientAtomicLongProxy::getAsync() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongGetCodec::encodeRequest(name);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongGetCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<int64_t> >
            ClientAtomicLongProxy::getAndAddAsync(int64_t delta) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongGetAndAddCodec::encodeRequest(name, delta);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongGetAndAddCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<int64_t> >
            ClientAtomicLongProxy::getAndSetAsync(int64_t newValue) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongGetAndSetCodec::encodeRequest(name, newValue);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongGetAndSetCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<int64_t> > ClientAtomicLongProxy::incrementAndGetAsync() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongIncrementAndGetCodec::encodeRequest(name);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongIncrementAndGetCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<int64_t> > ClientAtomicLongProxy::getAndIncrementAsync() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongGetAndIncrementCodec::encodeRequest(name);

                return invokeOnPartitionAsync<int64_t>(request,
                                                       impl::PrimitiveMessageDecoder<protocol::codec::AtomicLongGetAndIncrementCodec, int64_t>::instance());
            }

            std::shared_ptr<ICompletableFuture<void> > ClientAtomicLongProxy::setAsync(int64_t newValue) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::AtomicLongSetCodec::encodeRequest(name, newValue);

                return invokeOnPartitionAsync<void>(request, impl::VoidMessageDecoder::instance());
            }

            IMapImpl::IMapImpl(const std::string &instanceName, spi::ClientContext *context)
                    : ProxyImpl("hz:impl:mapService", instanceName, context) {
            }

            bool IMapImpl::containsKey(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapContainsKeyCodec::encodeRequest(getName(), key,
                                                                            util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MapContainsKeyCodec::ResponseParameters>(request,
                                                                                                          key);
            }

            bool IMapImpl::containsValue(const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapContainsValueCodec::encodeRequest(getName(), value);

                return invokeAndGetResult<bool, protocol::codec::MapContainsValueCodec::ResponseParameters>(request);
            }

            std::unique_ptr<serialization::pimpl::Data> IMapImpl::getData(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapGetCodec::encodeRequest(getName(), key, util::getCurrentThreadId());

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::MapGetCodec::ResponseParameters>(
                        request, key);
            }

            std::unique_ptr<serialization::pimpl::Data> IMapImpl::removeData(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapRemoveCodec::encodeRequest(getName(), key, util::getCurrentThreadId());

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::MapRemoveCodec::ResponseParameters>(
                        request, key);
            }

            bool IMapImpl::remove(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapRemoveIfSameCodec::encodeRequest(getName(), key, value,
                                                                             util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MapRemoveIfSameCodec::ResponseParameters>(request,
                                                                                                           key);
            }

            void IMapImpl::removeAll(const serialization::pimpl::Data &predicateData) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapRemoveAllCodec::encodeRequest(getName(), predicateData);

                invoke(request);
            }

            void IMapImpl::deleteEntry(const serialization::pimpl::Data &key) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapDeleteCodec::encodeRequest(getName(), key, util::getCurrentThreadId());

                invokeOnPartition(request, partitionId);
            }

            void IMapImpl::flush() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapFlushCodec::encodeRequest(getName());

                invoke(request);
            }

            bool IMapImpl::tryRemove(const serialization::pimpl::Data &key, int64_t timeoutInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapTryRemoveCodec::encodeRequest(getName(), key,
                                                                          util::getCurrentThreadId(),
                                                                          timeoutInMillis);

                return invokeAndGetResult<bool, protocol::codec::MapTryRemoveCodec::ResponseParameters>(request, key);
            }

            bool IMapImpl::tryPut(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value,
                                  int64_t timeoutInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapTryPutCodec::encodeRequest(getName(), key, value,
                                                                       util::getCurrentThreadId(),
                                                                       timeoutInMillis);

                return invokeAndGetResult<bool, protocol::codec::MapTryPutCodec::ResponseParameters>(request, key);
            }

            std::unique_ptr<serialization::pimpl::Data> IMapImpl::putData(const serialization::pimpl::Data &key,
                                                                          const serialization::pimpl::Data &value,
                                                                          int64_t ttlInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapPutCodec::encodeRequest(getName(), key, value,
                                                                    util::getCurrentThreadId(),
                                                                    ttlInMillis);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::MapPutCodec::ResponseParameters>(
                        request, key);
            }

            void IMapImpl::putTransient(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value,
                                        int64_t ttlInMillis) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapPutTransientCodec::encodeRequest(getName(), key, value,
                                                                             util::getCurrentThreadId(),
                                                                             ttlInMillis);

                invokeOnPartition(request, partitionId);
            }

            std::unique_ptr<serialization::pimpl::Data> IMapImpl::putIfAbsentData(const serialization::pimpl::Data &key,
                                                                                  const serialization::pimpl::Data &value,
                                                                                  int64_t ttlInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapPutIfAbsentCodec::encodeRequest(getName(), key, value,
                                                                            util::getCurrentThreadId(),
                                                                            ttlInMillis);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::MapPutIfAbsentCodec::ResponseParameters>(
                        request, key);
            }

            bool IMapImpl::replace(const serialization::pimpl::Data &key, const serialization::pimpl::Data &oldValue,
                                   const serialization::pimpl::Data &newValue) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapReplaceIfSameCodec::encodeRequest(getName(), key, oldValue,
                                                                              newValue,
                                                                              util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MapReplaceIfSameCodec::ResponseParameters>(request,
                                                                                                            key);
            }

            std::unique_ptr<serialization::pimpl::Data> IMapImpl::replaceData(const serialization::pimpl::Data &key,
                                                                              const serialization::pimpl::Data &value) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapReplaceCodec::encodeRequest(getName(), key, value,
                                                                        util::getCurrentThreadId());

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::MapReplaceCodec::ResponseParameters>(
                        request, key);
            }

            void IMapImpl::set(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value,
                               int64_t ttl) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapSetCodec::encodeRequest(getName(), key, value,
                                                                    util::getCurrentThreadId(), ttl);

                invokeOnPartition(request, partitionId);
            }

            void IMapImpl::lock(const serialization::pimpl::Data &key) {
                lock(key, -1);
            }

            void IMapImpl::lock(const serialization::pimpl::Data &key, int64_t leaseTime) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapLockCodec::encodeRequest(getName(), key, util::getCurrentThreadId(),
                                                                     leaseTime,
                                                                     lockReferenceIdGenerator->getNextReferenceId());

                invokeOnPartition(request, partitionId);
            }

            bool IMapImpl::isLocked(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapIsLockedCodec::encodeRequest(getName(), key);

                return invokeAndGetResult<bool, protocol::codec::MapIsLockedCodec::ResponseParameters>(request, key);
            }

            bool IMapImpl::tryLock(const serialization::pimpl::Data &key, int64_t timeInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapTryLockCodec::encodeRequest(getName(), key, util::getCurrentThreadId(), -1,
                                                                        timeInMillis,
                                                                        lockReferenceIdGenerator->getNextReferenceId());

                return invokeAndGetResult<bool, protocol::codec::MapTryLockCodec::ResponseParameters>(request, key);
            }

            void IMapImpl::unlock(const serialization::pimpl::Data &key) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapUnlockCodec::encodeRequest(getName(), key, util::getCurrentThreadId(),
                                                                       lockReferenceIdGenerator->getNextReferenceId());

                invokeOnPartition(request, partitionId);
            }

            void IMapImpl::forceUnlock(const serialization::pimpl::Data &key) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapForceUnlockCodec::encodeRequest(getName(), key,
                                                                            lockReferenceIdGenerator->getNextReferenceId());

                invokeOnPartition(request, partitionId);
            }

            std::string IMapImpl::addEntryListener(impl::BaseEventHandler *entryEventHandler, bool includeValue) {
                // TODO: Use appropriate flags for the event type as implemented in Java instead of EntryEventType::ALL
                int32_t listenerFlags = EntryEventType::ALL;
                return registerListener(createMapEntryListenerCodec(includeValue, listenerFlags), entryEventHandler);
            }

            std::string
            IMapImpl::addEntryListener(impl::BaseEventHandler *entryEventHandler, const query::Predicate &predicate,
                                       bool includeValue) {
                // TODO: Use appropriate flags for the event type as implemented in Java instead of EntryEventType::ALL
                int32_t listenerFlags = EntryEventType::ALL;
                serialization::pimpl::Data predicateData = toData<serialization::IdentifiedDataSerializable>(predicate);
                return registerListener(createMapEntryListenerCodec(includeValue, predicateData, listenerFlags),
                                        entryEventHandler);
            }

            bool IMapImpl::removeEntryListener(const std::string &registrationId) {
                return getContext().getClientListenerService().deregisterListener(registrationId);
            }

            std::string IMapImpl::addEntryListener(impl::BaseEventHandler *handler,
                                                   serialization::pimpl::Data &key, bool includeValue) {
                // TODO: Use appropriate flags for the event type as implemented in Java instead of EntryEventType::ALL
                int32_t listenerFlags = EntryEventType::ALL;
                return registerListener(createMapEntryListenerCodec(includeValue, listenerFlags, key), handler);

            }

            std::unique_ptr<map::DataEntryView> IMapImpl::getEntryViewData(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapGetEntryViewCodec::encodeRequest(getName(), key,
                                                                             util::getCurrentThreadId());

                return invokeAndGetResult<std::unique_ptr<map::DataEntryView>, protocol::codec::MapGetEntryViewCodec::ResponseParameters>(
                        request, key);
            }

            bool IMapImpl::evict(const serialization::pimpl::Data &key) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapEvictCodec::encodeRequest(getName(), key, util::getCurrentThreadId());

                return invokeAndGetResult<bool, protocol::codec::MapEvictCodec::ResponseParameters>(request,
                                                                                                    key);
            }

            void IMapImpl::evictAll() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapEvictAllCodec::encodeRequest(getName());

                invoke(request);
            }

            EntryVector
            IMapImpl::getAllData(const std::map<int, std::vector<serialization::pimpl::Data> > &partitionToKeyData) {
                std::vector<std::shared_ptr<spi::impl::ClientInvocationFuture> > futures;

                for (std::map<int, std::vector<serialization::pimpl::Data> >::const_iterator it = partitionToKeyData.begin();
                     it != partitionToKeyData.end(); ++it) {
                    std::unique_ptr<protocol::ClientMessage> request =
                            protocol::codec::MapGetAllCodec::encodeRequest(getName(), it->second);

                    futures.push_back(invokeAndGetFuture(request, it->first));
                }

                EntryVector result;
                // wait for all futures
                for (const std::shared_ptr<spi::impl::ClientInvocationFuture> &future : futures) {
                    std::shared_ptr<protocol::ClientMessage> responseForPartition = future->get();
                    protocol::codec::MapGetAllCodec::ResponseParameters resultForPartition =
                            protocol::codec::MapGetAllCodec::ResponseParameters::decode(
                                    *responseForPartition);
                    result.insert(result.end(), resultForPartition.response.begin(),
                                  resultForPartition.response.end());

                }

                return result;
            }

            std::vector<serialization::pimpl::Data> IMapImpl::keySetData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapKeySetCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MapKeySetCodec::ResponseParameters>(
                        request);
            }

            std::vector<serialization::pimpl::Data> IMapImpl::keySetData(
                    const serialization::IdentifiedDataSerializable &predicate) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapKeySetWithPredicateCodec::encodeRequest(getName(),
                                                                                    toData<serialization::IdentifiedDataSerializable>(
                                                                                            predicate));

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MapKeySetWithPredicateCodec::ResponseParameters>(
                        request);
            }

            std::vector<serialization::pimpl::Data> IMapImpl::keySetForPagingPredicateData(
                    const serialization::IdentifiedDataSerializable &predicate) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapKeySetWithPagingPredicateCodec::encodeRequest(getName(),
                                                                                          toData<serialization::IdentifiedDataSerializable>(
                                                                                                  predicate));

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MapKeySetWithPagingPredicateCodec::ResponseParameters>(
                        request);
            }

            EntryVector IMapImpl::entrySetData() {

                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapEntrySetCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<EntryVector, protocol::codec::MapEntrySetCodec::ResponseParameters>(
                        request);
            }

            EntryVector IMapImpl::entrySetData(
                    const serialization::IdentifiedDataSerializable &predicate) {

                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapEntriesWithPredicateCodec::encodeRequest(
                        getName(), toData(predicate));

                return invokeAndGetResult<EntryVector, protocol::codec::MapEntriesWithPredicateCodec::ResponseParameters>(
                        request);
            }

            EntryVector IMapImpl::entrySetForPagingPredicateData(
                    const serialization::IdentifiedDataSerializable &predicate) {

                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapEntriesWithPagingPredicateCodec::encodeRequest(
                        getName(), toData(predicate));

                return invokeAndGetResult<EntryVector, protocol::codec::MapEntriesWithPagingPredicateCodec::ResponseParameters>(
                        request);
            }

            std::vector<serialization::pimpl::Data> IMapImpl::valuesData() {

                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapValuesCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MapValuesCodec::ResponseParameters>(
                        request);
            }

            std::vector<serialization::pimpl::Data> IMapImpl::valuesData(
                    const serialization::IdentifiedDataSerializable &predicate) {

                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapValuesWithPredicateCodec::encodeRequest(
                        getName(), toData<serialization::IdentifiedDataSerializable>(predicate));

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::MapValuesWithPredicateCodec::ResponseParameters>(
                        request);
            }

            EntryVector
            IMapImpl::valuesForPagingPredicateData(const serialization::IdentifiedDataSerializable &predicate) {

                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapValuesWithPagingPredicateCodec::encodeRequest(
                        getName(), toData<serialization::IdentifiedDataSerializable>(predicate));

                return invokeAndGetResult<EntryVector, protocol::codec::MapValuesWithPagingPredicateCodec::ResponseParameters>(
                        request);
            }

            void IMapImpl::addIndex(const std::string &attribute, bool ordered) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapAddIndexCodec::encodeRequest(getName(), attribute, ordered);

                invoke(request);
            }

            int IMapImpl::size() {
                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapSizeCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<int, protocol::codec::MapSizeCodec::ResponseParameters>(request);
            }

            bool IMapImpl::isEmpty() {
                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::MapIsEmptyCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<bool, protocol::codec::MapIsEmptyCodec::ResponseParameters>(request);
            }

            void IMapImpl::putAllData(const std::map<int, EntryVector> &partitionedEntries) {
                std::vector<std::shared_ptr<spi::impl::ClientInvocationFuture> > futures;

                for (std::map<int, EntryVector>::const_iterator it = partitionedEntries.begin();
                     it != partitionedEntries.end(); ++it) {
                    std::unique_ptr<protocol::ClientMessage> request =
                            protocol::codec::MapPutAllCodec::encodeRequest(getName(), it->second);

                    futures.push_back(invokeAndGetFuture(request, it->first));
                }

                // wait for all futures
                for (const std::shared_ptr<spi::impl::ClientInvocationFuture> &future : futures) {
                    future->get();
                }
            }

            void IMapImpl::clear() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapClearCodec::encodeRequest(getName());

                invoke(request);
            }

            std::unique_ptr<serialization::pimpl::Data>
            IMapImpl::executeOnKeyData(const serialization::pimpl::Data &key,
                                       const serialization::pimpl::Data &processor) {
                int partitionId = getPartitionId(key);

                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapExecuteOnKeyCodec::encodeRequest(getName(),
                                                                             processor,
                                                                             key,
                                                                             util::getCurrentThreadId());

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>,
                        protocol::codec::MapExecuteOnKeyCodec::ResponseParameters>(request, partitionId);
            }

            EntryVector IMapImpl::executeOnKeysData(const std::vector<serialization::pimpl::Data> &keys,
                                                    const serialization::pimpl::Data &processor) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapExecuteOnKeysCodec::encodeRequest(getName(), processor, keys);

                return invokeAndGetResult<EntryVector,
                        protocol::codec::MapExecuteOnKeysCodec::ResponseParameters>(request);
            }

            std::string IMapImpl::addInterceptor(serialization::Portable &interceptor) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapAddInterceptorCodec::encodeRequest(
                                getName(), toData<serialization::Portable>(interceptor));

                return invokeAndGetResult<std::string, protocol::codec::MapAddInterceptorCodec::ResponseParameters>(
                        request);
            }

            std::string IMapImpl::addInterceptor(serialization::IdentifiedDataSerializable &interceptor) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapAddInterceptorCodec::encodeRequest(
                                getName(), toData<serialization::IdentifiedDataSerializable>(interceptor));

                return invokeAndGetResult<std::string, protocol::codec::MapAddInterceptorCodec::ResponseParameters>(
                        request);
            }

            void IMapImpl::removeInterceptor(const std::string &id) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::MapRemoveInterceptorCodec::encodeRequest(getName(), id);

                invoke(request);
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            IMapImpl::createMapEntryListenerCodec(bool includeValue, serialization::pimpl::Data &predicate,
                                                  int32_t listenerFlags) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new MapEntryListenerWithPredicateMessageCodec(getName(), includeValue, listenerFlags,
                                                                      predicate));
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            IMapImpl::createMapEntryListenerCodec(bool includeValue, int32_t listenerFlags) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new MapEntryListenerMessageCodec(getName(), includeValue, listenerFlags));
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            IMapImpl::createMapEntryListenerCodec(bool includeValue, int32_t listenerFlags,
                                                  serialization::pimpl::Data &key) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new MapEntryListenerToKeyCodec(getName(), includeValue, listenerFlags, key));
            }

            void IMapImpl::onInitialize() {
                ProxyImpl::onInitialize();

                lockReferenceIdGenerator = getContext().getLockReferenceIdGenerator();
            }

            IMapImpl::MapEntryListenerMessageCodec::MapEntryListenerMessageCodec(const std::string &name,
                                                                                 bool includeValue,
                                                                                 int32_t listenerFlags) : name(name),
                                                                                                          includeValue(
                                                                                                                  includeValue),
                                                                                                          listenerFlags(
                                                                                                                  listenerFlags) {}

            std::unique_ptr<protocol::ClientMessage>
            IMapImpl::MapEntryListenerMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::MapAddEntryListenerCodec::encodeRequest(name, includeValue, listenerFlags,
                                                                                localOnly);
            }

            std::string IMapImpl::MapEntryListenerMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::MapAddEntryListenerCodec::ResponseParameters::decode(responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            IMapImpl::MapEntryListenerMessageCodec::encodeRemoveRequest(const std::string &realRegistrationId) const {
                return protocol::codec::MapRemoveEntryListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool IMapImpl::MapEntryListenerMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::MapRemoveEntryListenerCodec::ResponseParameters::decode(clientMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            IMapImpl::MapEntryListenerToKeyCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::MapAddEntryListenerToKeyCodec::encodeRequest(name, key, includeValue,
                                                                                     listenerFlags, localOnly);
            }

            std::string IMapImpl::MapEntryListenerToKeyCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::MapAddEntryListenerToKeyCodec::ResponseParameters::decode(
                        responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            IMapImpl::MapEntryListenerToKeyCodec::encodeRemoveRequest(const std::string &realRegistrationId) const {
                return protocol::codec::MapRemoveEntryListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool IMapImpl::MapEntryListenerToKeyCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::MapRemoveEntryListenerCodec::ResponseParameters::decode(clientMessage).response;
            }

            IMapImpl::MapEntryListenerToKeyCodec::MapEntryListenerToKeyCodec(const std::string &name, bool includeValue,
                                                                             int32_t listenerFlags,
                                                                             const serialization::pimpl::Data &key)
                    : name(name), includeValue(includeValue), listenerFlags(listenerFlags), key(key) {}

            IMapImpl::MapEntryListenerWithPredicateMessageCodec::MapEntryListenerWithPredicateMessageCodec(
                    const std::string &name, bool includeValue, int32_t listenerFlags,
                    serialization::pimpl::Data &predicate) : name(name), includeValue(includeValue),
                                                             listenerFlags(listenerFlags), predicate(predicate) {}

            std::unique_ptr<protocol::ClientMessage>
            IMapImpl::MapEntryListenerWithPredicateMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::MapAddEntryListenerWithPredicateCodec::encodeRequest(name, predicate,
                                                                                             includeValue,
                                                                                             listenerFlags, localOnly);
            }

            std::string IMapImpl::MapEntryListenerWithPredicateMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::MapAddEntryListenerWithPredicateCodec::ResponseParameters::decode(
                        responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            IMapImpl::MapEntryListenerWithPredicateMessageCodec::encodeRemoveRequest(
                    const std::string &realRegistrationId) const {
                return protocol::codec::MapRemoveEntryListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool IMapImpl::MapEntryListenerWithPredicateMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::MapRemoveEntryListenerCodec::ResponseParameters::decode(clientMessage).response;
            }

            std::shared_ptr<spi::impl::ClientInvocationFuture>
            IMapImpl::putAsyncInternalData(int64_t ttl, const util::concurrent::TimeUnit &ttlUnit,
                                           const int64_t *maxIdle, const util::concurrent::TimeUnit &maxIdleUnit,
                                           const serialization::pimpl::Data &keyData,
                                           const serialization::pimpl::Data &valueData) {
                int64_t ttlMillis = hazelcast::util::TimeUtil::timeInMsOrOneIfResultIsZero(ttl, ttlUnit);
                std::unique_ptr<protocol::ClientMessage> request;
                if (maxIdle != NULL) {
                    request = protocol::codec::MapPutWithMaxIdleCodec::encodeRequest(name, keyData, valueData,
                                                                                     getCurrentThreadId(),
                                                                                     ttlMillis,
                                                                                     TimeUtil::timeInMsOrOneIfResultIsZero(
                                                                                             *maxIdle,
                                                                                             maxIdleUnit));
                } else {
                    request = protocol::codec::MapPutCodec::encodeRequest(name, keyData, valueData,
                                                                          getCurrentThreadId(),
                                                                          ttlMillis);
                }

                return invokeOnKeyOwner(request, keyData);
            }

            std::shared_ptr<spi::impl::ClientInvocationFuture>
            IMapImpl::setAsyncInternalData(int64_t ttl, const util::concurrent::TimeUnit &ttlUnit,
                                           const int64_t *maxIdle, const util::concurrent::TimeUnit &maxIdleUnit,
                                           const serialization::pimpl::Data &keyData,
                                           const serialization::pimpl::Data &valueData) {
                int64_t ttlMillis = TimeUtil::timeInMsOrOneIfResultIsZero(ttl, ttlUnit);
                std::unique_ptr<protocol::ClientMessage> request;
                if (maxIdle != NULL) {
                    request = protocol::codec::MapSetWithMaxIdleCodec::encodeRequest(name, keyData, valueData,
                                                                                     getCurrentThreadId(),
                                                                                     ttlMillis,
                                                                                     TimeUtil::timeInMsOrOneIfResultIsZero(
                                                                                             *maxIdle,
                                                                                             maxIdleUnit));
                } else {
                    request = protocol::codec::MapSetCodec::encodeRequest(name, keyData, valueData,
                                                                          getCurrentThreadId(),
                                                                          ttlMillis);
                }

                return invokeOnKeyOwner(request, keyData);
            }

            TransactionalQueueImpl::TransactionalQueueImpl(const std::string &name,
                                                           txn::TransactionProxy *transactionProxy)
                    : TransactionalObject("hz:impl:queueService", name, transactionProxy) {

            }

            bool TransactionalQueueImpl::offer(const serialization::pimpl::Data &e, long timeoutInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::TransactionalQueueOfferCodec::encodeRequest(
                                getName(), getTransactionId(), util::getCurrentThreadId(), e, timeoutInMillis);

                return invokeAndGetResult<bool, protocol::codec::TransactionalQueueOfferCodec::ResponseParameters>(
                        request);
            }

            std::unique_ptr<serialization::pimpl::Data> TransactionalQueueImpl::pollData(long timeoutInMillis) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::TransactionalQueuePollCodec::encodeRequest(
                                getName(), getTransactionId(), util::getCurrentThreadId(), timeoutInMillis);

                return invokeAndGetResult<std::unique_ptr<serialization::pimpl::Data>, protocol::codec::TransactionalQueuePollCodec::ResponseParameters>(
                        request);
            }

            int TransactionalQueueImpl::size() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::TransactionalQueueSizeCodec::encodeRequest(
                                getName(), getTransactionId(), util::getCurrentThreadId());

                return invokeAndGetResult<int, protocol::codec::TransactionalQueueSizeCodec::ResponseParameters>(
                        request);
            }

            ISetImpl::ISetImpl(const std::string &instanceName, spi::ClientContext *clientContext)
                    : ProxyImpl("hz:impl:setService", instanceName, clientContext) {
                serialization::pimpl::Data keyData = getContext().getSerializationService().toData<std::string>(
                        &instanceName);
                partitionId = getPartitionId(keyData);
            }

            std::string ISetImpl::addItemListener(impl::BaseEventHandler *itemEventHandler, bool includeValue) {
                return registerListener(createItemListenerCodec(includeValue), itemEventHandler);
            }

            bool ISetImpl::removeItemListener(const std::string &registrationId) {
                return getContext().getClientListenerService().deregisterListener(registrationId);
            }

            int ISetImpl::size() {
                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::SetSizeCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<int, protocol::codec::SetSizeCodec::ResponseParameters>(request, partitionId);
            }

            bool ISetImpl::isEmpty() {
                std::unique_ptr<protocol::ClientMessage> request = protocol::codec::SetIsEmptyCodec::encodeRequest(
                        getName());

                return invokeAndGetResult<bool, protocol::codec::SetIsEmptyCodec::ResponseParameters>(request,
                                                                                                      partitionId);
            }

            bool ISetImpl::contains(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetContainsCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::SetContainsCodec::ResponseParameters>(request,
                                                                                                       partitionId);
            }

            std::vector<serialization::pimpl::Data> ISetImpl::toArrayData() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetGetAllCodec::encodeRequest(getName());

                return invokeAndGetResult<std::vector<serialization::pimpl::Data>, protocol::codec::SetGetAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool ISetImpl::add(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetAddCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::SetAddCodec::ResponseParameters>(request, partitionId);
            }

            bool ISetImpl::remove(const serialization::pimpl::Data &element) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetRemoveCodec::encodeRequest(getName(), element);

                return invokeAndGetResult<bool, protocol::codec::SetRemoveCodec::ResponseParameters>(request,
                                                                                                     partitionId);
            }

            bool ISetImpl::containsAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetContainsAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::SetContainsAllCodec::ResponseParameters>(request,
                                                                                                          partitionId);
            }

            bool ISetImpl::addAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetAddAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::SetAddAllCodec::ResponseParameters>(request,
                                                                                                     partitionId);
            }

            bool ISetImpl::removeAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetCompareAndRemoveAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::SetCompareAndRemoveAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            bool ISetImpl::retainAll(const std::vector<serialization::pimpl::Data> &elements) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetCompareAndRetainAllCodec::encodeRequest(getName(), elements);

                return invokeAndGetResult<bool, protocol::codec::SetCompareAndRetainAllCodec::ResponseParameters>(
                        request, partitionId);
            }

            void ISetImpl::clear() {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::SetClearCodec::encodeRequest(getName());

                invokeOnPartition(request, partitionId);
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec>
            ISetImpl::createItemListenerCodec(bool includeValue) {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(
                        new SetListenerMessageCodec(getName(), includeValue));
            }

            ISetImpl::SetListenerMessageCodec::SetListenerMessageCodec(const std::string &name,
                                                                       bool includeValue) : name(name),
                                                                                            includeValue(
                                                                                                    includeValue) {}

            std::unique_ptr<protocol::ClientMessage>
            ISetImpl::SetListenerMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::SetAddListenerCodec::encodeRequest(name, includeValue, localOnly);
            }

            std::string ISetImpl::SetListenerMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::SetAddListenerCodec::ResponseParameters::decode(responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            ISetImpl::SetListenerMessageCodec::encodeRemoveRequest(const std::string &realRegistrationId) const {
                return protocol::codec::SetRemoveListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool ISetImpl::SetListenerMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::SetRemoveListenerCodec::ResponseParameters::decode(clientMessage).response;
            }

            ITopicImpl::ITopicImpl(const std::string &instanceName, spi::ClientContext *context)
                    : proxy::ProxyImpl("hz:impl:topicService", instanceName, context) {
                partitionId = getPartitionId(toData(instanceName));
            }

            void ITopicImpl::publish(const serialization::pimpl::Data &data) {
                std::unique_ptr<protocol::ClientMessage> request =
                        protocol::codec::TopicPublishCodec::encodeRequest(getName(), data);

                invokeOnPartition(request, partitionId);
            }

            std::string ITopicImpl::addMessageListener(impl::BaseEventHandler *topicEventHandler) {
                return registerListener(createItemListenerCodec(), topicEventHandler);
            }

            bool ITopicImpl::removeMessageListener(const std::string &registrationId) {
                return getContext().getClientListenerService().deregisterListener(registrationId);
            }

            std::shared_ptr<spi::impl::ListenerMessageCodec> ITopicImpl::createItemListenerCodec() {
                return std::shared_ptr<spi::impl::ListenerMessageCodec>(new TopicListenerMessageCodec(getName()));
            }

            ITopicImpl::TopicListenerMessageCodec::TopicListenerMessageCodec(const std::string &name) : name(name) {}

            std::unique_ptr<protocol::ClientMessage>
            ITopicImpl::TopicListenerMessageCodec::encodeAddRequest(bool localOnly) const {
                return protocol::codec::TopicAddMessageListenerCodec::encodeRequest(name, localOnly);
            }

            std::string ITopicImpl::TopicListenerMessageCodec::decodeAddResponse(
                    protocol::ClientMessage &responseMessage) const {
                return protocol::codec::TopicAddMessageListenerCodec::ResponseParameters::decode(
                        responseMessage).response;
            }

            std::unique_ptr<protocol::ClientMessage>
            ITopicImpl::TopicListenerMessageCodec::encodeRemoveRequest(const std::string &realRegistrationId) const {
                return protocol::codec::TopicRemoveMessageListenerCodec::encodeRequest(name, realRegistrationId);
            }

            bool ITopicImpl::TopicListenerMessageCodec::decodeRemoveResponse(
                    protocol::ClientMessage &clientMessage) const {
                return protocol::codec::TopicRemoveMessageListenerCodec::ResponseParameters::decode(
                        clientMessage).response;
            }
        }

        namespace map {
            DataEntryView::DataEntryView(const serialization::pimpl::Data &key, const serialization::pimpl::Data &value,
                                         int64_t cost,
                                         int64_t creationTime, int64_t expirationTime, int64_t hits,
                                         int64_t lastAccessTime,
                                         int64_t lastStoredTime, int64_t lastUpdateTime, int64_t version,
                                         int64_t evictionCriteriaNumber,
                                         int64_t ttl) : key(key), value(value), cost(cost), creationTime(creationTime),
                                                        expirationTime(expirationTime), hits(hits),
                                                        lastAccessTime(lastAccessTime),
                                                        lastStoredTime(lastStoredTime), lastUpdateTime(lastUpdateTime),
                                                        version(version),
                                                        evictionCriteriaNumber(evictionCriteriaNumber), ttl(ttl) {}


            const serialization::pimpl::Data &DataEntryView::getKey() const {
                return key;
            }

            const serialization::pimpl::Data &DataEntryView::getValue() const {
                return value;
            }

            int64_t DataEntryView::getCost() const {
                return cost;
            }

            int64_t DataEntryView::getCreationTime() const {
                return creationTime;
            }

            int64_t DataEntryView::getExpirationTime() const {
                return expirationTime;
            }

            int64_t DataEntryView::getHits() const {
                return hits;
            }

            int64_t DataEntryView::getLastAccessTime() const {
                return lastAccessTime;
            }

            int64_t DataEntryView::getLastStoredTime() const {
                return lastStoredTime;
            }

            int64_t DataEntryView::getLastUpdateTime() const {
                return lastUpdateTime;
            }

            int64_t DataEntryView::getVersion() const {
                return version;
            }

            int64_t DataEntryView::getEvictionCriteriaNumber() const {
                return evictionCriteriaNumber;
            }

            int64_t DataEntryView::getTtl() const {
                return ttl;
            }

        }

        namespace atomiclong {
            namespace impl {
                AtomicLongProxyFactory::AtomicLongProxyFactory(spi::ClientContext *clientContext) : clientContext(
                        clientContext) {}

                std::shared_ptr<spi::ClientProxy> AtomicLongProxyFactory::create(const std::string &id) {
                    return std::shared_ptr<spi::ClientProxy>(new proxy::ClientAtomicLongProxy(id, clientContext));
                }
            }
        }

        namespace topic {
            namespace impl {
                namespace reliable {
                    ReliableTopicExecutor::ReliableTopicExecutor(Ringbuffer<ReliableTopicMessage> &rb,
                                                                 util::ILogger &logger)
                            : ringbuffer(rb),
                              runnerThread(std::shared_ptr<util::Runnable>(new Task(ringbuffer, q, shutdown)), logger),
                              q(10), shutdown(false) {
                    }

                    ReliableTopicExecutor::~ReliableTopicExecutor() {
                        stop();
                    }

                    void ReliableTopicExecutor::start() {
                        runnerThread.start();
                    }

                    void ReliableTopicExecutor::stop() {
                        bool expected = false;
                        if (!shutdown.compare_exchange_strong(expected, true)) {
                            return;
                        }

                        topic::impl::reliable::ReliableTopicExecutor::Message m;
                        m.type = topic::impl::reliable::ReliableTopicExecutor::CANCEL;
                        m.callback = NULL;
                        m.sequence = -1;
                        execute(m);
                        runnerThread.join();
                    }

                    void ReliableTopicExecutor::execute(const Message &m) {
                        q.push(m);
                    }

                    void ReliableTopicExecutor::Task::run() {
                        while (!shutdown) {
                            Message m = q.pop();
                            if (CANCEL == m.type) {
                                // exit the thread
                                return;
                            }
                            try {
                                proxy::ClientRingbufferProxy<ReliableTopicMessage> &ringbuffer =
                                        static_cast<proxy::ClientRingbufferProxy<ReliableTopicMessage> &>(rb);
                                std::shared_ptr<spi::impl::ClientInvocationFuture> future = ringbuffer.readManyAsync(
                                        m.sequence, 1, m.maxCount);
                                std::shared_ptr<protocol::ClientMessage> responseMsg;
                                do {
                                    if (future->get(1000, TimeUnit::MILLISECONDS())) {
                                        responseMsg = future->get(); // every one second
                                    }
                                } while (!shutdown && (protocol::ClientMessage *) NULL == responseMsg.get());

                                if (!shutdown) {
                                    std::shared_ptr<DataArray<ReliableTopicMessage> > allMessages(
                                            ringbuffer.getReadManyAsyncResponseObject(
                                                    responseMsg));

                                    m.callback->onResponse(allMessages);
                                }
                            } catch (exception::IException &e) {
                                m.callback->onFailure(std::shared_ptr<exception::IException>(e.clone()));
                            }
                        }
                    }

                    const std::string ReliableTopicExecutor::Task::getName() const {
                        return "ReliableTopicExecutor Task";
                    }

                    ReliableTopicExecutor::Task::Task(Ringbuffer<ReliableTopicMessage> &rb,
                                                      util::BlockingConcurrentQueue<ReliableTopicExecutor::Message> &q,
                                                      util::AtomicBoolean &shutdown) : rb(rb), q(q),
                                                                                       shutdown(shutdown) {}

                    ReliableTopicMessage::ReliableTopicMessage() {
                    }

                    ReliableTopicMessage::ReliableTopicMessage(
                            hazelcast::client::serialization::pimpl::Data payloadData,
                            std::unique_ptr<Address> &address)
                            : publishTime(util::currentTimeMillis()), publisherAddress(std::move(address)),
                              payload(payloadData) {
                    }

                    int64_t ReliableTopicMessage::getPublishTime() const {
                        return publishTime;
                    }

                    const Address *ReliableTopicMessage::getPublisherAddress() const {
                        return publisherAddress.get();
                    }

                    const serialization::pimpl::Data &ReliableTopicMessage::getPayload() const {
                        return payload;
                    }

                    int ReliableTopicMessage::getFactoryId() const {
                        return F_ID;
                    }

                    int ReliableTopicMessage::getClassId() const {
                        return RELIABLE_TOPIC_MESSAGE;
                    }

                    void ReliableTopicMessage::writeData(serialization::ObjectDataOutput &out) const {
                        out.writeLong(publishTime);
                        out.writeObject<serialization::IdentifiedDataSerializable>(publisherAddress.get());
                        out.writeData(&payload);
                    }

                    void ReliableTopicMessage::readData(serialization::ObjectDataInput &in) {
                        publishTime = in.readLong();
                        publisherAddress = in.readObject<Address>();
                        payload = in.readData();
                    }
                }
            }
        }

        ICountDownLatch::ICountDownLatch(const std::string &objectName, spi::ClientContext *context)
                : proxy::ProxyImpl("hz:impl:atomicLongService", objectName, context) {
            serialization::pimpl::Data keyData = context->getSerializationService().toData<std::string>(&objectName);
            partitionId = getPartitionId(keyData);
        }

        bool ICountDownLatch::await(long timeoutInMillis) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::CountDownLatchAwaitCodec::encodeRequest(getName(), timeoutInMillis);

            return invokeAndGetResult<bool, protocol::codec::CountDownLatchAwaitCodec::ResponseParameters>(request,
                                                                                                           partitionId);
        }

        void ICountDownLatch::countDown() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::CountDownLatchCountDownCodec::encodeRequest(getName());

            invokeOnPartition(request, partitionId);
        }

        int ICountDownLatch::getCount() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::CountDownLatchGetCountCodec::encodeRequest(getName());

            return invokeAndGetResult<int, protocol::codec::CountDownLatchGetCountCodec::ResponseParameters>(request,
                                                                                                             partitionId);
        }

        bool ICountDownLatch::trySetCount(int count) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::CountDownLatchTrySetCountCodec::encodeRequest(getName(), count);

            return invokeAndGetResult<bool, protocol::codec::CountDownLatchTrySetCountCodec::ResponseParameters>(
                    request, partitionId);
        }

        EntryEventType::EntryEventType() : value(UNDEFINED) {
        }

        EntryEventType::EntryEventType(Type value)
                : value(value) {
        }

        EntryEventType::operator int() const {
            return value;
        }

        void EntryEventType::operator=(int i) {
            value = (EntryEventType::Type) i;
        }

        MapEvent::MapEvent(const Member &member, EntryEventType eventType, const std::string &name,
                           int numberOfEntriesAffected)
                : member(member), eventType(eventType), name(name), numberOfEntriesAffected(numberOfEntriesAffected) {

        }

        Member MapEvent::getMember() const {
            return member;
        }

        EntryEventType MapEvent::getEventType() const {
            return eventType;
        }

        const std::string &MapEvent::getName() const {
            return name;
        }

        int MapEvent::getNumberOfEntriesAffected() const {
            return numberOfEntriesAffected;
        }

        std::ostream &operator<<(std::ostream &os, const MapEvent &event) {
            os << "MapEvent{member: " << event.member << " eventType: " << event.eventType << " name: " << event.name
               << " numberOfEntriesAffected: " << event.numberOfEntriesAffected;
            return os;
        }

        ItemEventType::ItemEventType() {
        }

        ItemEventType::ItemEventType(Type value) : value(value) {
        }

        ItemEventType::operator int() const {
            return value;
        }

        /**
         * copy function.
         */
        void ItemEventType::operator=(int i) {
            switch (i) {
                case 1:
                    value = ADDED;
                    break;
                case 2:
                    value = REMOVED;
                    break;
            }
        }

        ItemEventBase::ItemEventBase(const std::string &name, const Member &member, const ItemEventType &eventType)
                : name(name),
                  member(member),
                  eventType(
                          eventType) {}

        Member ItemEventBase::getMember() const {
            return member;
        }

        ItemEventType ItemEventBase::getEventType() const {
            return eventType;
        }

        std::string ItemEventBase::getName() const {
            return name;
        }

        ItemEventBase::~ItemEventBase() {
        }

        bool IdGenerator::init(int64_t id) {
            return impl->init(id);
        }

        int64_t IdGenerator::newId() {
            return impl->newId();
        }

        IdGenerator::IdGenerator(const std::shared_ptr<impl::IdGeneratorInterface> &impl) : impl(impl) {}

        IdGenerator::~IdGenerator() {
        }

        int64_t IAtomicLong::addAndGet(int64_t delta) {
            return impl->addAndGet(delta);
        }

        bool IAtomicLong::compareAndSet(int64_t expect, int64_t update) {
            return impl->compareAndSet(expect, update);
        }

        int64_t IAtomicLong::decrementAndGet() {
            return impl->decrementAndGet();
        }

        int64_t IAtomicLong::get() {
            return impl->get();
        }

        int64_t IAtomicLong::getAndAdd(int64_t delta) {
            return impl->getAndAdd(delta);
        }

        int64_t IAtomicLong::getAndSet(int64_t newValue) {
            return impl->getAndSet(newValue);
        }

        int64_t IAtomicLong::incrementAndGet() {
            return impl->incrementAndGet();
        }

        int64_t IAtomicLong::getAndIncrement() {
            return impl->getAndIncrement();
        }

        void IAtomicLong::set(int64_t newValue) {
            impl->set(newValue);
        }

        IAtomicLong::IAtomicLong(const std::shared_ptr<impl::AtomicLongInterface> &impl) : impl(impl) {}

        const std::string &IAtomicLong::getServiceName() const {
            return impl->getServiceName();
        }

        const std::string &IAtomicLong::getName() const {
            return impl->getName();
        }

        void IAtomicLong::destroy() {
            impl->destroy();
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::addAndGetAsync(int64_t delta) {
            return impl->addAndGetAsync(delta);
        }

        std::shared_ptr<ICompletableFuture<bool> > IAtomicLong::compareAndSetAsync(int64_t expect, int64_t update) {
            return impl->compareAndSetAsync(expect, update);
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::decrementAndGetAsync() {
            return impl->decrementAndGetAsync();
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::getAsync() {
            return impl->getAsync();
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::getAndAddAsync(int64_t delta) {
            return impl->getAndAddAsync(delta);
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::getAndSetAsync(int64_t newValue) {
            return impl->getAndSetAsync(newValue);
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::incrementAndGetAsync() {
            return impl->incrementAndGetAsync();
        }

        std::shared_ptr<ICompletableFuture<int64_t> > IAtomicLong::getAndIncrementAsync() {
            return impl->getAndIncrementAsync();
        }

        std::shared_ptr<ICompletableFuture<void> > IAtomicLong::setAsync(int64_t newValue) {
            return impl->setAsync(newValue);
        }

        ILock::ILock(const std::string &instanceName, spi::ClientContext *context)
                : proxy::ProxyImpl("hz:impl:lockService", instanceName, context),
                  key(toData<std::string>(instanceName)) {
            partitionId = getPartitionId(key);

            // TODO: remove this line once the client instance getDistributedObject works as expected in Java for this proxy type
            referenceIdGenerator = context->getLockReferenceIdGenerator();
        }

        void ILock::lock() {
            lock(-1);
        }

        void ILock::lock(long leaseTimeInMillis) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockLockCodec::encodeRequest(getName(), leaseTimeInMillis,
                                                                  util::getCurrentThreadId(),
                                                                  referenceIdGenerator->getNextReferenceId());

            invokeOnPartition(request, partitionId);
        }

        void ILock::unlock() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockUnlockCodec::encodeRequest(getName(), util::getCurrentThreadId(),
                                                                    referenceIdGenerator->getNextReferenceId());

            invokeOnPartition(request, partitionId);
        }

        void ILock::forceUnlock() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockForceUnlockCodec::encodeRequest(getName(),
                                                                         referenceIdGenerator->getNextReferenceId());

            invokeOnPartition(request, partitionId);
        }

        bool ILock::isLocked() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockIsLockedCodec::encodeRequest(getName());

            return invokeAndGetResult<bool, protocol::codec::LockIsLockedCodec::ResponseParameters>(request,
                                                                                                    partitionId);
        }

        bool ILock::isLockedByCurrentThread() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockIsLockedByCurrentThreadCodec::encodeRequest(getName(),
                                                                                     util::getCurrentThreadId());

            return invokeAndGetResult<bool, protocol::codec::LockIsLockedByCurrentThreadCodec::ResponseParameters>(
                    request, partitionId);
        }

        int ILock::getLockCount() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockGetLockCountCodec::encodeRequest(getName());

            return invokeAndGetResult<int, protocol::codec::LockGetLockCountCodec::ResponseParameters>(request,
                                                                                                       partitionId);
        }

        long ILock::getRemainingLeaseTime() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockGetRemainingLeaseTimeCodec::encodeRequest(getName());

            return invokeAndGetResult<long, protocol::codec::LockGetRemainingLeaseTimeCodec::ResponseParameters>(
                    request, partitionId);
        }

        bool ILock::tryLock() {
            return tryLock(0);
        }

        bool ILock::tryLock(long timeInMillis) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::LockTryLockCodec::encodeRequest(getName(), util::getCurrentThreadId(), LONG_MAX,
                                                                     timeInMillis,
                                                                     referenceIdGenerator->getNextReferenceId());

            return invokeAndGetResult<bool, protocol::codec::LockTryLockCodec::ResponseParameters>(request,
                                                                                                   partitionId);
        }

        void ILock::onInitialize() {
            ProxyImpl::onInitialize();

            referenceIdGenerator = getContext().getLockReferenceIdGenerator();
        }

        FlakeIdGenerator::FlakeIdGenerator(const std::shared_ptr<impl::IdGeneratorInterface> &impl) : IdGenerator(
                impl) {}

        int64_t FlakeIdGenerator::newId() {
            return IdGenerator::newId();
        }

        bool FlakeIdGenerator::init(int64_t id) {
            return IdGenerator::init(id);
        }

        ISemaphore::ISemaphore(const std::string &name, spi::ClientContext *context)
                : proxy::ProxyImpl("hz:impl:semaphoreService", name, context) {
            serialization::pimpl::Data keyData = context->getSerializationService().toData<std::string>(&name);
            partitionId = getPartitionId(keyData);
        }

        bool ISemaphore::init(int permits) {
            checkNegative(permits);
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreInitCodec::encodeRequest(getName(), permits);

            return invokeAndGetResult<bool, protocol::codec::SemaphoreInitCodec::ResponseParameters>(request,
                                                                                                     partitionId);
        }

        void ISemaphore::acquire() {
            acquire(1);
        }

        void ISemaphore::acquire(int permits) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreAcquireCodec::encodeRequest(getName(), permits);

            invokeOnPartition(request, partitionId);
        }

        int ISemaphore::availablePermits() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreAvailablePermitsCodec::encodeRequest(getName());

            return invokeAndGetResult<int, protocol::codec::SemaphoreAvailablePermitsCodec::ResponseParameters>(request,
                                                                                                                partitionId);
        }

        int ISemaphore::drainPermits() {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreDrainPermitsCodec::encodeRequest(getName());

            return invokeAndGetResult<int, protocol::codec::SemaphoreDrainPermitsCodec::ResponseParameters>(request,
                                                                                                            partitionId);
        }

        void ISemaphore::reducePermits(int reduction) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreReducePermitsCodec::encodeRequest(getName(), reduction);

            invokeOnPartition(request, partitionId);
        }

        void ISemaphore::release() {
            release(1);
        }

        void ISemaphore::release(int permits) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreReleaseCodec::encodeRequest(getName(), permits);

            invokeOnPartition(request, partitionId);
        }

        bool ISemaphore::tryAcquire() {
            return tryAcquire(int(1));
        }

        bool ISemaphore::tryAcquire(int permits) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreTryAcquireCodec::encodeRequest(getName(), permits, 0);

            return invokeAndGetResult<bool, protocol::codec::SemaphoreTryAcquireCodec::ResponseParameters>(request,
                                                                                                           partitionId);
        }

        bool ISemaphore::tryAcquire(long timeoutInMillis) {
            return tryAcquire(1, timeoutInMillis);
        }

        bool ISemaphore::tryAcquire(int permits, long timeoutInMillis) {
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreTryAcquireCodec::encodeRequest(getName(), permits, timeoutInMillis);

            return invokeAndGetResult<bool, protocol::codec::SemaphoreTryAcquireCodec::ResponseParameters>(request,
                                                                                                           partitionId);
        }

        void ISemaphore::increasePermits(int32_t increase) {
            checkNegative(increase);
            std::unique_ptr<protocol::ClientMessage> request =
                    protocol::codec::SemaphoreIncreasePermitsCodec::encodeRequest(getName(), increase);
            invokeOnPartition(request, partitionId);
        }

        void ISemaphore::checkNegative(int32_t permits) const {
            if (permits < 0) {
                throw exception::IllegalArgumentException("ISemaphore::checkNegative", "Permits cannot be negative!");
            }
        }

    }
}
