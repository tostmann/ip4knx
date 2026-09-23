#include <string.h>

#include "table_object.h"
#include "bits.h"
#include "memory.h"
#include "callback_property.h"
#include "data_property.h"

BeforeTablesUnloadCallback TableObject::_beforeTablesUnload = 0;
uint8_t TableObject::_tableUnloadCount = 0;

void TableObject::beforeTablesUnloadCallback(BeforeTablesUnloadCallback func)
{
    _beforeTablesUnload = func;
}

BeforeTablesUnloadCallback TableObject::beforeTablesUnloadCallback()
{
    return _beforeTablesUnload;
}

TableObject::TableObject(Memory& memory, uint32_t staticTableAdr , uint32_t staticTableSize)
    : _memory(memory)
{
    _staticTableAdr = staticTableAdr;
    _staticTableSize = staticTableSize;
}

TableObject::~TableObject()
{}

void TableObject::beforeStateChange(LoadState& newState)
{
    if (newState == LS_LOADED && _tableUnloadCount > 0)
        _tableUnloadCount--;
    if (_tableUnloadCount > 0)
        return;
    if (newState == LS_UNLOADED) {
        _tableUnloadCount++;
        if (_beforeTablesUnload != 0)
            _beforeTablesUnload();
    }
}

LoadState TableObject::loadState()
{
    return _state;
}

void TableObject::loadState(LoadState newState)
{
    if (newState == _state)
        return;
    beforeStateChange(newState);
    _state = newState;
    // The load state belongs to the stored configuration.
    _memory.scheduleSave();
}


uint8_t* TableObject::save(uint8_t* buffer)
{
    //println("TableObject::save");
    allocTableStatic();

    buffer = pushByte(_state, buffer);

    buffer = pushInt(_size, buffer);

    if (_data)
        buffer = pushInt(_memory.toRelative(_data), buffer);
    else
        buffer = pushInt(0, buffer);

    return InterfaceObject::save(buffer);
}


const uint8_t* TableObject::restore(const uint8_t* buffer)
{
    //println("TableObject::restore");

    uint8_t state = 0;
    buffer = popByte(state, buffer);
    // Only the states loadEvent() dispatches on: any other value froze the object,
    // and every later load event, Unload included, was ignored.
    _state = (state == LS_UNLOADED || state == LS_LOADED || state == LS_LOADING || state == LS_ERROR)
                 ? (LoadState)state : LS_UNLOADED;

    buffer = popInt(_size, buffer);

    uint32_t relativeAddress = 0;
    buffer = popInt(relativeAddress, buffer);
    //println(relativeAddress);

    if (_staticTableAdr)
    {
        // A static table's address and size are build constants, not stored state.
        // Memory::readMemory() hands the pointer to addNewUsedBlock(), so one that
        // does not fit the NVM is left at zero and the table is skipped.
        if (staticTableFitsNvm())
        {
            _size = _staticTableSize;
            _data = _memory.toAbsolute(_staticTableAdr);
        }
        else
        {
            _size = 0;
            _data = 0;
        }
    }
    else if (relativeAddress != 0)
        _data = _memory.toAbsolute(relativeAddress);
    else
        _data = 0;
    //println((uint32_t)_data);
    return InterfaceObject::restore(buffer);
}

uint32_t TableObject::tableReference()
{
    // Zero when there is no allocation, instead of 0 minus the NVM start.
    if (_data == nullptr)
        return 0;

    return (uint32_t)_memory.toRelative(_data);
}

bool TableObject::allocTable(uint32_t size, bool doFill, uint8_t fillByte)
{
    if(_staticTableAdr)
        return false;

    if (_data)
    {
        _memory.freeMemory(_data);
        _data = 0;
    }

    if (size == 0)
        return true;

    _data = _memory.allocMemory(size);
    if (!_data)
        return false;

    if (doFill)
    {
        uint32_t addr = _memory.toRelative(_data);
        for(uint32_t i = 0; i < size;i++)
            _memory.writeMemory(addr+i, 1, &fillByte);
    }

    _size = size;

    return true;
}


// The address and the extent of a static table are build constants and were never
// compared against the NVM. The 091A filter table sits at 0x200 with 0x2000 octets,
// far beyond the 1024-octet NVM this firmware has, so the first Memory::writeMemory()
// -- run by every A_Restart -- ended in the fatalError() of addNewUsedBlock(): the KNX
// side stopped for good while WiFi and the web interface kept running.
bool TableObject::staticTableFitsNvm()
{
    return (uint32_t)_staticTableAdr + _staticTableSize <= _memory.memorySize();
}

void TableObject::allocTableStatic()
{
    if(_staticTableAdr && !_data)
    {
        if (!staticTableFitsNvm())
            return;

        _data = _memory.toAbsolute(_staticTableAdr);
        _size = _staticTableSize;
        if (!_memory.addNewUsedBlock(_data, _size))
        {
            _data = 0;
            _size = 0;
        }
    }
}

void TableObject::loadEvent(const uint8_t* data)
{
    //printHex("TableObject::loadEvent 0x", data, 10);
    switch (_state)
    {
        case LS_UNLOADED:
            loadEventUnloaded(data);
            break;
        case LS_LOADING:
            loadEventLoading(data);
            break;
        case LS_LOADED:
            loadEventLoaded(data);
            break;
        case LS_ERROR:
            loadEventError(data);
            break;
        default:
            /* do nothing */
            break;
    }
}

void TableObject::loadEventUnloaded(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_LOAD_COMPLETED:
        case LE_ADDITIONAL_LOAD_CONTROLS:
        case LE_UNLOAD:
            break;
        case LE_START_LOADING:
            loadState(LS_LOADING);
            break;
        default:
            loadState(LS_ERROR);
            errorCode(E_GOT_UNDEF_LOAD_CMD);
    }
}

void TableObject::loadEventLoading(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_START_LOADING:
            break;
        case LE_LOAD_COMPLETED:
            // A table without memory must not report Loaded: its consumers read
            // data(). A static table gets its block here when no save has placed
            // it yet; one that does not fit the NVM ends in Error.
            allocTableStatic();
            if (_data == nullptr)
            {
                loadState(LS_ERROR);
                errorCode(E_GOT_MEM_ALLOC_ZERO);
                break;
            }
            // No raw commit of the NVM buffer here: loadState() schedules a full
            // save, which writes the buffer together with the metadata.
            loadState(LS_LOADED);
            break;
        case LE_UNLOAD:
            loadState(LS_UNLOADED);
            break;
        case LE_ADDITIONAL_LOAD_CONTROLS:
            additionalLoadControls(data);
            break;
        default:
            loadState(LS_ERROR);
            errorCode(E_GOT_UNDEF_LOAD_CMD);
    }
}

void TableObject::loadEventLoaded(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_LOAD_COMPLETED:
            break;
        case LE_START_LOADING:
            loadState(LS_LOADING);
            break;
        case LE_UNLOAD:
            loadState(LS_UNLOADED);
            //free nv memory
            if (_data)
            {
                if(!_staticTableAdr)
                {
                    _memory.freeMemory(_data);
                    _data = 0;
                }
            }
            break;
        case LE_ADDITIONAL_LOAD_CONTROLS:
            loadState(LS_ERROR);
            errorCode(E_INVALID_OPCODE);
            break;
        default:
            loadState(LS_ERROR);
            errorCode(E_GOT_UNDEF_LOAD_CMD);
    }
}

void TableObject::loadEventError(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_LOAD_COMPLETED:
        case LE_ADDITIONAL_LOAD_CONTROLS:
        case LE_START_LOADING:
            break;
        case LE_UNLOAD:
            loadState(LS_UNLOADED);
            break;
        default:
            loadState(LS_ERROR);
            errorCode(E_GOT_UNDEF_LOAD_CMD);
    }
}

void TableObject::additionalLoadControls(const uint8_t* data)
{
    if (data[1] != 0x0B) // Data Relative Allocation
    {
        loadState(LS_ERROR);
        errorCode(E_INVALID_OPCODE);
        return;
    }

    size_t size = ((data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5]);
    bool doFill = data[6] == 0x1;
    uint8_t fillByte = data[7];
    if (!allocTable(size, doFill, fillByte))
    {
        loadState(LS_ERROR);
        errorCode(E_MAX_TABLE_LENGTH_EXEEDED);
    }
}

uint8_t* TableObject::data()
{
    return _data;
}

void TableObject::errorCode(ErrorCode errorCode)
{
    // Only a dynamic table has PID_ERROR_CODE (initializeDynTableProperties); a static
    // one -- both tables of the 091A -- gets its properties from
    // InterfaceObject::initializeProperties. Without the check a single
    // A_PropertyValue_Write of an unknown load event to object 1 or 2 wrote through
    // a null pointer. (upstream OpenKNX/knx adacceb)
    Property* prop = property(PID_ERROR_CODE);
    if (prop == nullptr)
        return;

    uint8_t data = errorCode;
    prop->write(data);
}

uint16_t TableObject::saveSize()
{
    return 5 + InterfaceObject::saveSize() + sizeof(_size);
}

void TableObject::initializeProperties(size_t propertiesSize, Property** properties)
{
    Property* ownProperties[] =
    {
        new CallbackProperty<TableObject>(this, PID_LOAD_STATE_CONTROL, true, PDT_CONTROL, 1, ReadLv3 | WriteLv3,
            [](TableObject* obj, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                data[0] = obj->_state;
                return 1;
            },
            [](TableObject* obj, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t {
                obj->loadEvent(data);
                return 1;
            })
     };

    uint8_t ownPropertiesCount = sizeof(ownProperties) / sizeof(Property*);

    uint8_t propertyCount = propertiesSize / sizeof(Property*);
    uint8_t allPropertiesCount = propertyCount + ownPropertiesCount;

    Property* allProperties[allPropertiesCount];
    memcpy(allProperties, properties, propertiesSize);
    memcpy(allProperties + propertyCount, ownProperties, sizeof(ownProperties));

    if(_staticTableAdr)
        InterfaceObject::initializeProperties(sizeof(allProperties), allProperties);
    else
        initializeDynTableProperties(sizeof(allProperties), allProperties);
}

void TableObject::initializeDynTableProperties(size_t propertiesSize, Property** properties)
{
    Property* ownProperties[] =
    {
        new CallbackProperty<TableObject>(this, PID_TABLE_REFERENCE, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](TableObject* obj, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                if (obj->_state == LS_UNLOADED)
                    pushInt(0, data);
                else
                    pushInt(obj->tableReference(), data);
                return 1;
            }),
        new CallbackProperty<TableObject>(this, PID_MCB_TABLE, false, PDT_GENERIC_08, 1, ReadLv3 | WriteLv0,
            [](TableObject* obj, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if (obj->_state != LS_LOADED)
                    return 0; // need to check return code for invalid
                
                uint32_t segmentSize = obj->_size;
                uint16_t crc16 = crc16Ccitt(obj->data(), segmentSize); 

                pushInt(segmentSize, data);     // Segment size
                pushByte(0x00, data + 4);       // CRC control byte -> 0: always valid
                pushByte(0xFF, data + 5);       // Read access 4 bits + Write access 4 bits
                pushWord(crc16, data + 6);      // CRC-16 CCITT of data
    
                return 1;
            }),
        new DataProperty(PID_ERROR_CODE, false, PDT_ENUM8, 1, ReadLv3 | WriteLv0, (uint8_t)E_NO_FAULT)
     };

    uint8_t ownPropertiesCount = sizeof(ownProperties) / sizeof(Property*);

    uint8_t propertyCount = propertiesSize / sizeof(Property*);
    uint8_t allPropertiesCount = propertyCount + ownPropertiesCount;

    Property* allProperties[allPropertiesCount];
    memcpy(allProperties, properties, propertiesSize);
    memcpy(allProperties + propertyCount, ownProperties, sizeof(ownProperties));

    InterfaceObject::initializeProperties(sizeof(allProperties), allProperties);
}