#include "memory.h"

#include <string.h>

#include "bits.h"

Memory::Memory(Platform& platform, DeviceObject& deviceObject)
    : _platform(platform), _deviceObject(deviceObject)
{}

Memory::~Memory()
{}

void Memory::readMemory()
{
    println("readMemory");

    uint8_t* flashStart = _platform.getNonVolatileMemoryStart();
    size_t flashSize = _platform.getNonVolatileMemorySize();
    if (flashStart == nullptr)
    {
        println("no user flash available;");
        return;
    }

    printHex("RESTORED ", flashStart, _metadataSize);

    uint16_t metadataBlockSize = alignToPageSize(_metadataSize);

    _freeList = new MemoryBlock(flashStart + metadataBlockSize, flashSize - metadataBlockSize);

    uint16_t apiVersion = 0;
    const uint8_t* buffer = popWord(apiVersion, flashStart);

    uint16_t layoutWord = 0;
    buffer = popWord(layoutWord, buffer);

    uint16_t manufacturerId = 0;
    buffer = popWord(manufacturerId, buffer);

    uint8_t hardwareType[LEN_HARDWARE_TYPE] = {0};
    buffer = popByteArray(hardwareType, LEN_HARDWARE_TYPE, buffer);

    uint16_t version = 0;
    buffer = popWord(version, buffer);

    VersionCheckResult versionCheck = FlashAllInvalid;

    // first check correct format of deviceObject-API
    if (_deviceObject.apiVersion == apiVersion) 
    {
        // The records are read back by position, without lengths or identities. An
        // image from a build whose stored layout differs -- a property added, an
        // array resized -- would be parsed field by field into the wrong places.
        if (layoutWord != layoutFingerprint())
        {
            println("stored layout belongs to a different firmware build");
            print("expected layout: ");
            print(layoutFingerprint(), HEX);
            print(", stored layout: ");
            println(layoutWord, HEX);
        }
        else if (_versionCheckCallback != 0) {
            versionCheck = _versionCheckCallback(manufacturerId, hardwareType, version);
            // callback should provide infomation about version check failure reasons
        }
        else if (_deviceObject.manufacturerId() == manufacturerId &&
                 memcmp(_deviceObject.hardwareType(), hardwareType, LEN_HARDWARE_TYPE) == 0) 
        {
            if (_deviceObject.version() == version) {
                versionCheck = FlashValid;
            } 
            else
            {
                versionCheck = FlashTablesInvalid;
            }
        } 
        else 
        {
            println("manufacturerId or hardwareType are different");
            print("expexted manufacturerId: ");
            print(_deviceObject.manufacturerId(), HEX);
            print(", stored manufacturerId: ");
            println(manufacturerId, HEX);
            print("expexted hardwareType: ");
            printHex("", _deviceObject.hardwareType(), LEN_HARDWARE_TYPE);
            print(", stored hardwareType: ");
            printHex("", hardwareType, LEN_HARDWARE_TYPE);
            println("");
        }
    } 
    else 
    {
        println("DataObject api changed, any data stored in flash is invalid.");
        print("expexted DataObject api version: ");
        print(_deviceObject.apiVersion, HEX);
        print(", stored api version: ");
        println(apiVersion, HEX);
    }

    if (versionCheck == FlashAllInvalid)
    {
        println("ETS has to reprogram PA and application!");
        return;
    }

    println("restoring data from flash...");
    print("Restore saveRestores: ");
    println(_saveCount);
    for (int i = 0; i < _saveCount; i++)
    {
        buffer = _saveRestores[i]->restore(buffer);
    }
    println("Restored saveRestores");

    // Upstream OpenKNX/knx a83b291 (2026-02-24): on 091A, load TableObjects
    // even when FlashTablesInvalid (firmware version mismatch) and fill the
    // data with 0xff. Without this, an ETS reprogramming after firmware
    // update fails because the table objects never get constructed in RAM.
#if MASK_VERSION == 0x091A
    if(versionCheck == FlashTablesInvalid)
    {
        println("TableObjects are referring to an older firmware version and are restored, unloaded and filled with 0xff");
    }
#else
    if (versionCheck == FlashTablesInvalid)
    {
        println("TableObjects are referring to an older firmware version and are not restored");
        return;
    }
#endif
    print("Restore TableObjs: ");
    println(_tableObjCount);
    for (int i = 0; i < _tableObjCount; i++)
    {
        buffer = _tableObjects[i]->restore(buffer);
        uint16_t memorySize = 0;
        buffer = popWord(memorySize, buffer);
        print("Size: ");
        println(memorySize);
        // The address and the size of a dynamic table come from the stored image,
        // which management writes can reach; a static table takes both from its build
        // constants (TableObject::restore). A block outside the free NVM is dropped
        // with its table instead of being registered: fatalError() here stopped every
        // boot. A table left without memory does not report Loaded.
        TableObject* table = _tableObjects[i];
        if (table->_staticTableAdr)
            memorySize = (table->_data != nullptr) ? table->_size : 0;

        if (memorySize == 0 || table->_data == nullptr || !addNewUsedBlock(table->_data, memorySize))
        {
            if (memorySize != 0)
                println("stored table outside the NVM -- not restored");
            table->_data = 0;
            table->_size = 0;
            if (table->_state == LS_LOADED)
                table->_state = LS_UNLOADED;
            continue;
        }
        if (table->_size > memorySize)
            table->_size = memorySize;

#if MASK_VERSION == 0x091A
        // load the tables but delete the data
        if(versionCheck == FlashTablesInvalid)
        {
            println("unload and fill with 0xff");
            _tableObjects[i]->loadState(LS_UNLOADED);
            uint32_t start = toRelative(_tableObjects[i]->_data);
            uint8_t fillByte = 0xff;
            uint32_t end = start + _tableObjects[i]->_size;
            for(uint32_t j = start; j < end; j++)
                writeMemory(j, 1, &fillByte);
        }
#endif
    }
    println("restored TableObjects");
}

// The word in the NVM header that identifies the stored layout of this build: each
// registered record contributes its kind, its length and the identity of the
// properties it writes, in registration order. Computed on demand, because
// RouterObject fills its property table in initialize(), after registration.
uint16_t Memory::layoutFingerprint()
{
    uint32_t hash = 2166136261u; // FNV-1a offset basis

    for (int i = 0; i < _saveCount; i++)
        hash = mixRecord(hash, 0x0001, _saveRestores[i]);

    for (int i = 0; i < _tableObjCount; i++)
        hash = mixRecord(hash, 0x0002, _tableObjects[i]);

    return (uint16_t)((hash >> 16) ^ (hash & 0xFFFF));
}

uint32_t Memory::mixRecord(uint32_t hash, uint16_t kind, SaveRestore* obj)
{
    const uint32_t tag = obj->layoutTag();

    hash = fnv1aWord(hash, kind);
    hash = fnv1aWord(hash, obj->saveSize());
    hash = fnv1aWord(hash, (uint16_t)(tag >> 16));
    hash = fnv1aWord(hash, (uint16_t)tag);
    return hash;
}

void Memory::writeMemory()
{
    // first get the necessary size of the writeBuffer
    uint16_t writeBufferSize = _metadataSize;
    for (int i = 0; i < _saveCount; i++)
        writeBufferSize = MAX(writeBufferSize, _saveRestores[i]->saveSize());

    for (int i = 0; i < _tableObjCount; i++)
        writeBufferSize = MAX(writeBufferSize, _tableObjects[i]->saveSize() + 2 /*for memory pos*/);
    
    uint8_t buffer[writeBufferSize];
    uint32_t flashPos = 0;
    uint8_t* bufferPos = buffer;

    bufferPos = pushWord(_deviceObject.apiVersion, bufferPos);
    bufferPos = pushWord(layoutFingerprint(), bufferPos);
    bufferPos = pushWord(_deviceObject.manufacturerId(), bufferPos);
    bufferPos = pushByteArray(_deviceObject.hardwareType(), LEN_HARDWARE_TYPE, bufferPos);
    bufferPos = pushWord(_deviceObject.version(), bufferPos);

    flashPos = _platform.writeNonVolatileMemory(flashPos, buffer, bufferPos - buffer);

    print("save saveRestores ");
    println(_saveCount);
    for (int i = 0; i < _saveCount; i++)
    {
        bufferPos = _saveRestores[i]->save(buffer);
        flashPos = _platform.writeNonVolatileMemory(flashPos, buffer, bufferPos - buffer);
    }

    print("save tableobjs ");
    println(_tableObjCount);
    for (int i = 0; i < _tableObjCount; i++)
    {
        bufferPos = _tableObjects[i]->save(buffer);

        //save to size of the memoryblock for tableobject too, so that we can rebuild the usedList and freeList
        if (_tableObjects[i]->_data != nullptr)
        {
            MemoryBlock* block = findBlockInList(_usedList, _tableObjects[i]->_data);
            if (block == nullptr)
            {
                println("_data of TableObject not in _usedList");
                _platform.fatalError();
            }
            bufferPos = pushWord(block->size, bufferPos);
        }
        else
            bufferPos = pushWord(0, bufferPos);

        flashPos = _platform.writeNonVolatileMemory(flashPos, buffer, bufferPos - buffer);
    }
    
    _platform.commitNonVolatileMemory();
}

void Memory::saveMemory()
{
    _platform.commitNonVolatileMemory();
}

// Store the configuration five seconds after the last change (Memory::loop()).
// Until now only freeMemory() armed the timer, which a static table never reaches,
// so on the 091A nothing was stored except on A_Restart.
void Memory::scheduleSave()
{
    if (_saveTimeout == 0)
    {
        // first change since the last save
        _firstPendingChange = millis();
        if (_firstPendingChange == 0)
            _firstPendingChange = 1;
    }
    _saveTimeout = millis();
    if (_saveTimeout == 0)
        _saveTimeout = 1; // prevent 0=disabled
}

void Memory::addSaveRestore(SaveRestore* obj)
{
    if (_saveCount >= MAXSAVE - 1)
        return;

    _saveRestores[_saveCount] = obj;
    _saveCount += 1;
    _metadataSize += obj->saveSize();
}

void Memory::addSaveRestore(TableObject* obj)
{
    if (_tableObjCount >= MAXTABLEOBJ)
        return;

    _tableObjects[_tableObjCount] = obj;
    _tableObjCount += 1;
    _metadataSize += obj->saveSize();
    _metadataSize += 2; // for size
}

uint8_t* Memory::allocMemory(size_t size)
{
    // always allocate aligned to pagesize
    size = alignToPageSize(size);

    MemoryBlock* freeBlock = _freeList;
    MemoryBlock* blockToUse = nullptr;
    
    // find the smallest possible block that is big enough
    while (freeBlock)
    {
        if (freeBlock->size >= size)
        {
            if (blockToUse != nullptr && (blockToUse->size - size) > (freeBlock->size - size))
                blockToUse = freeBlock;
            else if (blockToUse == nullptr)
                blockToUse = freeBlock;
        }
        freeBlock = freeBlock->next;
    }
    if (!blockToUse)
    {
        println("No available non volatile memory!");
        _platform.fatalError();
    }

    if (blockToUse->size == size)
    {
        // use whole block
        removeFromFreeList(blockToUse);
        addToUsedList(blockToUse);
        return blockToUse->address;
    }
    else
    {
        // split block
        MemoryBlock* newBlock = new MemoryBlock(blockToUse->address, size);
        addToUsedList(newBlock);

        blockToUse->address += size;
        blockToUse->size -= size;

        return newBlock->address;
    }
}


void Memory::freeMemory(uint8_t* ptr)
{
    MemoryBlock* block = _usedList;
    MemoryBlock* found = nullptr;
    while (block)
    {
        if (block->address == ptr)
        {
            found = block;
            break;
        }
        block = block->next;
    }
    if(!found)
    {
        println("freeMemory for not used pointer called");
        _platform.fatalError();
    }
    removeFromUsedList(block);
    addToFreeList(block);
    scheduleSave();
}

void Memory::writeMemory(uint32_t relativeAddress, size_t size, uint8_t* data)
{
    // EC: bounds-check against the NVM size (wrap-safe) -> a management write (Memory/User/Ext-MemoryWrite),
    // now reachable over the IP tunnel, must never write outside NVM (flash corruption / eeprom-buffer OOB).
    const size_t nvmSize = _platform.getNonVolatileMemorySize();
    if (size > nvmSize || relativeAddress > nvmSize - size)
        return;
    if(_saveTimeout != 0)
    {
        _saveTimeout = millis();
        if (_saveTimeout == 0)
            _saveTimeout = 1; // prevent 0=disabled; no impact by minimal increased timeout
    }
    _platform.writeNonVolatileMemory(relativeAddress, data, size);
}

void Memory::readMemory(uint32_t relativeAddress, size_t size, uint8_t* data)
{
    // EC: same wrap-safe bounds check on the read side (no OOB read of NVM).
    const size_t nvmSize = _platform.getNonVolatileMemorySize();
    if (size > nvmSize || relativeAddress > nvmSize - size)
        return;
    _platform.readNonVolatileMemory(relativeAddress, data, size);
}


uint8_t* Memory::toAbsolute(uint32_t relativeAddress)
{
    return _platform.getNonVolatileMemoryStart() + (ptrdiff_t)relativeAddress;
}

uint8_t* Memory::toAbsoluteChecked(uint32_t relativeAddress, size_t size)
{
    // Wrap-safe NVM bound (same as read/writeMemory): reject an out-of-range range so a management
    // memory-read cannot memcpy past the NVM buffer (OOB read / info-leak). Returns nullptr on reject.
    const size_t nvmSize = _platform.getNonVolatileMemorySize();
    if (size > nvmSize || relativeAddress > nvmSize - size)
        return nullptr;
    return toAbsolute(relativeAddress);
}


uint32_t Memory::toRelative(uint8_t* absoluteAddress)
{
    return absoluteAddress - _platform.getNonVolatileMemoryStart();
}

size_t Memory::memorySize()
{
    return _platform.getNonVolatileMemorySize();
}

MemoryBlock* Memory::removeFromList(MemoryBlock* head, MemoryBlock* item)
{
    if (head == item)
    {
        MemoryBlock* newHead = head->next;
        head->next = nullptr;
        return newHead;
    }

    if (!head || !item)
    {
        println("invalid parameters of Memory::removeFromList");
        _platform.fatalError();
    }

    bool found = false;
    MemoryBlock* block = head;
    while (block)
    {
        if (block->next == item)
        {
            found = true;
            block->next = item->next;
            break;
        }
        block = block->next;
    }

    if (!found)
    {
        println("tried to remove block from list not in it");
        _platform.fatalError();
    }
    item->next = nullptr;
    return head;
}

void Memory::removeFromFreeList(MemoryBlock* block)
{
    _freeList = removeFromList(_freeList, block);
}


void Memory::removeFromUsedList(MemoryBlock* block)
{
    _usedList = removeFromList(_usedList, block);
}


void Memory::addToUsedList(MemoryBlock* block)
{
    block->next = _usedList;
    _usedList = block;
}


void Memory::addToFreeList(MemoryBlock* block)
{
    if (_freeList == nullptr)
    {
        _freeList = block;
        return;
    }

    // first insert free block in list
    MemoryBlock* current = _freeList;
    while (current)
    {
        if (current->address <= block->address && (current->next == nullptr || block->address < current->next->address))
        {
            //add after current
            block->next = current->next;
            current->next = block;
            break;
        }
        else if (current->address > block->address)
        {
            //add before current
            block->next = current;

            if (current == _freeList)
                _freeList = block;

            // swap current and block for merge
            MemoryBlock* tmp = current;
            current = block;
            block = tmp;

            break;
        }

        current = current->next;
    }
    // now check if we can merge the blocks
    // first current an block
    if ((current->address + current->size) == block->address)
    {
        current->size += block->size;
        current->next = block->next;
        delete block;
        // check further if now current can be merged with current->next
        block = current;
    }

    // if block is the last one, we are done 
    if (block->next == nullptr)
        return;

    // now check block and block->next
    if ((block->address + block->size) == block->next->address)
    {
        block->size += block->next->size;
        block->next = block->next->next;
        delete block->next;
    }
}

uint16_t Memory::alignToPageSize(size_t size)
{
    size_t pageSize = 4; //_platform.flashPageSize(); // align to 32bit for now, as aligning to flash-page-size causes side effects in programming
    // pagesize should be a multiply of two
    return (size + pageSize - 1) & (-1*pageSize);
}

MemoryBlock* Memory::findBlockInList(MemoryBlock* head, uint8_t* address)
{
    while (head != nullptr)
    {
        if (head->address == address)
            return head;

        head = head->next;
    }
    return nullptr;
}

// Returns false, and changes nothing, when the block does not lie inside one free
// block. Both callers can drop the table instead: halting here stopped the device.
bool Memory::addNewUsedBlock(uint8_t* address, size_t size)
{
    MemoryBlock* smallerFreeBlock = _freeList;
    // find block in freeList where the new used block is contained in
    while (smallerFreeBlock)
    {
        if (smallerFreeBlock->next == nullptr ||
            (smallerFreeBlock->next != nullptr && smallerFreeBlock->next->address > address))
            break;
        
        smallerFreeBlock = smallerFreeBlock->next;
    }

    if (smallerFreeBlock == nullptr)
    {
        println("addNewUsedBlock: no smallerBlock found");
        return false;
    }

    // Compared as integers: the address may come from a stored image and point
    // anywhere, before the block as well as past its end.
    uintptr_t blockStart = (uintptr_t)smallerFreeBlock->address;
    uintptr_t blockEnd = blockStart + smallerFreeBlock->size;
    uintptr_t start = (uintptr_t)address;
    if (size == 0 || start < blockStart || start > blockEnd || size > blockEnd - start)
    {
        println("addNewUsedBlock: found block can't contain new block");
        return false;
    }

    if (smallerFreeBlock->address == address && smallerFreeBlock->size == size)
    {
        // we take thow whole block
        removeFromFreeList(smallerFreeBlock);
        addToUsedList(smallerFreeBlock);
        return true;
    }

    if (smallerFreeBlock->address == address)
    {
        // we take a front part of the block
        smallerFreeBlock->address += size;
        smallerFreeBlock->size -= size;
    }
    else
    {
        // we take a middle or end part of the block
        uint8_t* oldEndAddr = smallerFreeBlock->address + smallerFreeBlock->size;
        smallerFreeBlock->size = (address - smallerFreeBlock->address);

        if (address + size < oldEndAddr)
        {
            // we take the middle part of the block, so we need a new free block for the end part
            MemoryBlock* newFreeBlock = new MemoryBlock();
            newFreeBlock->next = smallerFreeBlock->next;
            newFreeBlock->address = address + size;
            newFreeBlock->size = oldEndAddr - newFreeBlock->address;
            smallerFreeBlock->next = newFreeBlock;
        }
    }

    MemoryBlock* newUsedBlock = new MemoryBlock(address, size);
    addToUsedList(newUsedBlock);
    return true;
}

void Memory::versionCheckCallback(VersionCheckCallback func)
{
    _versionCheckCallback = func;
}

VersionCheckCallback Memory::versionCheckCallback()
{
    return _versionCheckCallback;
}

// A timed save runs five seconds after the last change, and at most once a minute:
// every accepted property write schedules one, and management has no
// authentication, so without the spacing a client could force a flash write
// every five seconds. A change is at most a minute late on flash.
static const unsigned long kMinSaveIntervalMs = 60000;

void Memory::loop()
{
    if (_saveTimeout == 0)
        return;

    const unsigned long now = millis();
    // Due after five quiet seconds, or once the oldest unsaved change is a minute
    // old: every change restarts the five seconds, so a client writing more often
    // than that held the save off for as long as it kept writing, and a change made
    // in between -- an individual address -- never reached flash.
    const bool due = (now - _saveTimeout > 5000) || (now - _firstPendingChange >= kMinSaveIntervalMs);
    const bool spaced = (_lastSave == 0 || now - _lastSave >= kMinSaveIntervalMs);
    if (due && spaced)
    {
        println("saveMemory timeout");
        _saveTimeout = 0;
        _firstPendingChange = 0;
        writeMemory();
        _lastSave = millis();
        if (_lastSave == 0)
            _lastSave = 1; // 0 means no save yet
    }
}