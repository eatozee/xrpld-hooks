#include <ripple/app/tx/applyHook.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <memory>
#include <string>
#include <optional>
#include <any>
#include <vector>
#include "support/span.h"

using namespace ripple;

#define HR_ACC() hookResult.account << "-" << hookResult.otxnAccount
#define HC_ACC() hookCtx.result.account << "-" << hookCtx.result.otxnAccount

#define COMPUTE_HOOK_DATA_OWNER_COUNT(state_count)\
    (std::ceil( (double)state_count/(double)5.0 ))

#define HOOK_SETUP()\
    [[maybe_unused]] ApplyContext& applyCtx = hookCtx.applyCtx;\
    [[maybe_unused]] auto& view = applyCtx.view();\
    [[maybe_unused]] auto j = applyCtx.app.journal("View");\
    [[maybe_unused]] unsigned char* memory = memoryCtx.getPointer<uint8_t*>(0);\
    [[maybe_unused]] const uint64_t memory_length = memoryCtx.getDataPageSize() * memoryCtx.kPageSize;

#define WRITE_WASM_MEMORY(bytes_written, guest_dst_ptr, guest_dst_len,\
        host_src_ptr, host_src_len, host_memory_ptr, guest_memory_length)\
{\
    int64_t bytes_to_write = std::min(static_cast<int64_t>(host_src_len), static_cast<int64_t>(guest_dst_len));\
    if (guest_dst_ptr + bytes_to_write > guest_memory_length)\
    {\
        JLOG(j.warn())\
            << "HookError[" << HC_ACC() << "]: "\
            << __func__ << " tried to retreive blob of " << host_src_len\
            << " bytes past end of wasm memory";\
        return OUT_OF_BOUNDS;\
    }\
    memoryCtx.setBytes(SSVM::Span<const uint8_t>((const uint8_t*)host_src_ptr, host_src_len), \
            guest_dst_ptr, 0, bytes_to_write);\
    bytes_written += bytes_to_write;\
}

#define WRITE_WASM_MEMORY_AND_RETURN(guest_dst_ptr, guest_dst_len,\
        host_src_ptr, host_src_len, host_memory_ptr, guest_memory_length)\
{\
    int64_t bytes_written = 0;\
    WRITE_WASM_MEMORY(bytes_written, guest_dst_ptr, guest_dst_len, host_src_ptr,\
            host_src_len, host_memory_ptr, guest_memory_length);\
    return bytes_written;\
}

#define RETURN_HOOK_TRACE(read_ptr, read_len, t)\
{\
    int rl = read_len;\
    if (rl > 1024)\
        rl = 1024;\
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))\
    {\
        return OUT_OF_BOUNDS;\
    }\
    else if (read_ptr == 0 && read_len == 0)\
    {\
        JLOG(j.trace()) \
            << "HookTrace[" << HC_ACC() << "]: " << t;\
    }\
    else if (is_UTF16LE(memory + read_ptr, rl))\
    {\
        uint8_t output[1024];\
        int len = rl / 2;\
        for (int i = 0; i < len && i < 512; ++i)\
            output[i] = memory[read_ptr + i * 2];\
        JLOG(j.trace()) \
            << "HookTrace[" << HC_ACC() << "]: "\
            << std::string_view((const char*)output, (size_t)(len)) << " "\
            << t;\
    }\
    else\
    {\
        JLOG(j.trace()) \
            << "HookTrace[" << HC_ACC() << "]: "\
            << std::string_view((const char*)(memory + read_ptr), (size_t)rl) << " "\
            << t;\
    }\
    return 0;\
}
// ptr = pointer inside the wasm memory space
#define NOT_IN_BOUNDS(ptr, len, memory_length)\
    (ptr > memory_length || \
     static_cast<uint64_t>(ptr) + static_cast<uint64_t>(len) > static_cast<uint64_t>(memory_length))

#define HOOK_EXIT(read_ptr, read_len, error_code, exit_type)\
{\
    if (read_len > 64) read_len = 64;\
    if (read_ptr) {\
        if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length)) {\
            JLOG(j.warn())\
                << "HookError[" << HC_ACC() << "]: "\
                << "Tried to accept/rollback but specified memory outside of the wasm instance " <<\
                "limit when specifying a reason string";\
            return OUT_OF_BOUNDS;\
        }\
        /* assembly script and some other languages use utf16 for strings */\
        if (is_UTF16LE(read_ptr + memory, read_len))\
        {\
            uint8_t output[32];\
            int len = read_len / 2; /* is_UTF16LE will only return true if read_len is even */\
            for (int i = 0; i < len; ++i)\
                output[i] = memory[read_ptr + i * 2];\
            hookCtx.result.exitReason = std::string((const char*)(output), (size_t)len);\
        } else\
            hookCtx.result.exitReason = std::string((const char*)(memory + read_ptr), (size_t)read_len);\
    }\
    hookCtx.result.exitType = exit_type;\
    hookCtx.result.exitCode = error_code;\
    return (exit_type == hook_api::ExitType::ACCEPT ? RC_ACCEPT : RC_ROLLBACK);\
}
namespace hook_float
{
    using namespace hook_api;
    static int64_t const minMantissa = 1000000000000000ull;
    static int64_t const maxMantissa = 9999999999999999ull;
    static int32_t const minExponent = -96;
    static int32_t const maxExponent = 80;
    inline int32_t get_exponent(int64_t float1)
    {
        if (float1 < 0)
            return INVALID_FLOAT;
        if (float1 == 0)
            return 0;
        if (float1 < 0) return INVALID_FLOAT;
        uint64_t float_in = (uint64_t)float1;
        float_in >>= 54U;
        float_in &= 0xFFU;
        return ((int32_t)float_in) - 97;
    }

    inline uint64_t get_mantissa(int64_t float1)
    {
        if (float1 < 0)
            return INVALID_FLOAT;
        if (float1 == 0)
            return 0;
        if (float1 < 0) return INVALID_FLOAT;
        float1 -= ((((uint64_t)float1) >> 54U) << 54U);
        return float1;
    }

    inline bool is_negative(int64_t float1)
    {
        return ((float1 >> 62U) & 1ULL) == 0;
    }

    inline int64_t invert_sign(int64_t float1)
    {
        int64_t r = (int64_t)(((uint64_t)float1) ^ (1ULL<<62U));
        return r;
    }

    inline int64_t set_sign(int64_t float1, bool set_negative)
    {
        bool neg = is_negative(float1);
        if ((neg && set_negative) || (!neg && !set_negative))
            return float1;

        return invert_sign(float1);
    }

    inline int64_t set_mantissa(int64_t float1, uint64_t mantissa)
    {
        if (mantissa > maxMantissa)
            return MANTISSA_OVERSIZED;
        return float1 - get_mantissa(float1) + mantissa;
    }

    inline int64_t set_exponent(int64_t float1, int32_t exponent)
    {
        if (exponent > maxExponent)
            return EXPONENT_OVERSIZED;
        if (exponent < minExponent)
            return EXPONENT_UNDERSIZED;

        uint64_t exp = (exponent + 97);
        exp <<= 54U;
        float1 &= ~(0xFFLL<<54);
        float1 += (int64_t)exp;
        return float1;
    }

    inline int64_t make_float(ripple::IOUAmount& amt)
    {
        int64_t man_out = amt.mantissa();
        int64_t float_out = 0;
        bool neg = man_out < 0;
        if (neg)
            man_out *= -1;

        float_out = set_sign(float_out, neg);
        float_out = set_mantissa(float_out, man_out);
        float_out = set_exponent(float_out, amt.exponent());
        return float_out;
    }

    inline int64_t make_float(int64_t mantissa, int32_t exponent)
    {
        if (mantissa == 0)
            return 0;
        if (mantissa > maxMantissa)
            return MANTISSA_OVERSIZED;
        if (exponent > maxExponent)
            return EXPONENT_OVERSIZED;
        if (exponent < minExponent)
            return EXPONENT_UNDERSIZED;
        bool neg = mantissa < 0;
        if (neg)
            mantissa *= -1LL;
        int64_t out =  0;
        out = set_mantissa(out, mantissa);
        out = set_exponent(out, exponent);
        out = set_sign(out, neg);
        return out;
    }

    inline int64_t float_set(int32_t exp, int64_t mantissa)
    {
        if (mantissa == 0)
            return 0;

        bool neg = mantissa < 0;
        if (neg)
            mantissa *= -1LL;

        // normalize
        while (mantissa < minMantissa)
        {
            mantissa *= 10;
            exp--;
            if (exp < minExponent)
                return INVALID_FLOAT; //underflow
        }
        while (mantissa > maxMantissa)
        {
            mantissa /= 10;
            exp++;
            if (exp > maxExponent)
                return INVALID_FLOAT; //overflow
        }

        return make_float( ( neg ? -1LL : 1LL ) * mantissa, exp);

    }

}
using namespace hook_float;
inline
int32_t
no_free_slots(hook::HookContext& hookCtx)
{
    return (hookCtx.slot_counter > hook_api::max_slots && hookCtx.slot_free.size() == 0);
}


inline
int32_t
get_free_slot(hook::HookContext& hookCtx)
{
    int32_t slot_into = 0;

    // allocate a slot
    if (hookCtx.slot_free.size() > 0)
    {
        slot_into = hookCtx.slot_free.front();
        hookCtx.slot_free.pop();
    }

    // no slots were available in the queue so increment slot counter
    if (slot_into == 0)
        slot_into = hookCtx.slot_counter++;

    return slot_into;
}

inline int64_t
serialize_keylet(
        ripple::Keylet& kl,
        uint8_t* memory, uint32_t write_ptr, uint32_t write_len)
{
    if (write_len < 34)
        return hook_api::TOO_SMALL;

    memory[write_ptr + 0] = (kl.type >> 8) & 0xFFU;
    memory[write_ptr + 1] = (kl.type >> 0) & 0xFFU;

    for (int i = 0; i < 32; ++i)
        memory[write_ptr + 2 + i] = kl.key.data()[i];

    return 34;
}

std::optional<ripple::Keylet>
unserialize_keylet(uint8_t* ptr, uint32_t len)
{
    if (len != 34)
        return std::nullopt;

    uint16_t ktype =
        ((uint16_t)ptr[0] << 8) +
        ((uint16_t)ptr[1]);

    ripple::Keylet reconstructed { (ripple::LedgerEntryType)ktype, ripple::uint256::fromVoid(ptr + 2) };
    return reconstructed;
}


// RH TODO: this is used by sethook to determine the value stored in ltHOOK
// replace this with votable value
int hook::maxHookStateDataSize(void) {
    return 128;
}

bool hook::isEmittedTxn(ripple::STTx const& tx)
{
    return tx.isFieldPresent(ripple::sfEmitDetails);
}
// many datatypes can be encoded into an int64_t
inline int64_t data_as_int64(
        void* ptr_raw,
        uint32_t len)
{
    unsigned char* ptr = reinterpret_cast<unsigned char*>(ptr_raw);
    if (len > 8)
        return hook_api::hook_return_code::TOO_BIG;
    uint64_t output = 0;
    for (int i = 0, j = (len-1)*8; i < len; ++i, j-=8)
        output += (((uint64_t)ptr[i]) << j);
    if ((1ULL<<63) & output)
        return hook_api::hook_return_code::TOO_BIG;
    return output;
}

/* returns true iff every even char is ascii and every odd char is 00
 * only a hueristic, may be inaccurate in edgecases */
inline bool is_UTF16LE(const uint8_t* buffer, size_t len)
{
    if (len % 2 != 0 || len == 0)
        return false;

    for (int i = 0; i < len; i+=2)
        if (buffer[i + 0] == 0 || buffer[i + 1] != 0)
            return false;

    return true;
}


// Called by Transactor.cpp to determine if a transaction type can trigger a given hook...
// The HookOn field in the SetHook transaction determines which transaction types (tt's) trigger the hook.
// Every bit except ttHookSet is active low, so for example ttESCROW_FINISH = 2, so if the 2nd bit (counting from 0)
// from the right is 0 then the hook will trigger on ESCROW_FINISH. If it is 1 then ESCROW_FINISH will not trigger
// the hook. However ttHOOK_SET = 22 is active high, so by default (HookOn == 0) ttHOOK_SET is not triggered by
// transactions. If you wish to set a hook that has control over ttHOOK_SET then set bit 1U<<22.
bool hook::canHook(ripple::TxType txType, uint64_t hookOn) {
    // invert ttHOOK_SET bit
    hookOn ^= (1ULL << ttHOOK_SET);
    // invert entire field
    hookOn ^= 0xFFFFFFFFFFFFFFFFULL;
    return (hookOn >> txType) & 1;
}


// Update HookState ledger objects for the hook... only called after accept() or reject()
TER
hook::setHookState(
    HookResult& hookResult,
    ripple::ApplyContext& applyCtx,
    Keylet const& hookStateKeylet,
    ripple::uint256 key,
    Slice& data
){

    auto& view = applyCtx.view();
    auto j = applyCtx.app.journal("View");
    auto const sle = view.peek(hookResult.accountKeylet);
    if (!sle)
        return tefINTERNAL;

    auto const hook = view.peek(hookResult.hookKeylet);
    if (!hook) {
        JLOG(j.warn())
            << "HookError[" << HR_ACC() << "]: "
            << "Attempted to set a hook state for a hook that doesnt exist";
        return tefINTERNAL;
    }

    uint32_t hookDataMax = hook->getFieldU32(sfHookStateDataMaxSize);

    // if the blob is too large don't set it
    if (data.size() > hookDataMax)
       return temHOOK_DATA_TOO_LARGE;

    uint32_t stateCount = hook->getFieldU32(sfHookStateCount);
    uint32_t oldStateReserve = COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount);

    auto const oldHookState = view.peek(hookStateKeylet);

    // if the blob is nil then delete the entry if it exists
    if (data.size() == 0)
    {

        if (!view.peek(hookStateKeylet))
            return tesSUCCESS; // a request to remove a non-existent entry is defined as success

        auto const hint = (*oldHookState)[sfOwnerNode];

        // Remove the node from the account directory.
        if (!view.dirRemove(hookResult.ownerDirKeylet, hint, hookStateKeylet.key, false))
        {
            return tefBAD_LEDGER;
        }

        // remove the actual hook state obj
        view.erase(oldHookState);

        // adjust state object count
        if (stateCount > 0)
            --stateCount; // guard this because in the "impossible" event it is already 0 we'll wrap back to int_max

        // if removing this state entry would destroy the allotment then reduce the owner count
        if (COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount) < oldStateReserve)
            adjustOwnerCount(view, sle, -1, j);

        hook->setFieldU32(sfHookStateCount, COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount));

        return tesSUCCESS;
    }


    std::uint32_t ownerCount{(*sle)[sfOwnerCount]};

    if (oldHookState) {
        view.erase(oldHookState);
    } else {

        ++stateCount;

        if (COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount) > oldStateReserve) {
            // the hook used its allocated allotment of state entries for its previous ownercount
            // increment ownercount and give it another allotment

            ++ownerCount;
            XRPAmount const newReserve{
                view.fees().accountReserve(ownerCount)};

            if (STAmount((*sle)[sfBalance]).xrp() < newReserve)
                return tecINSUFFICIENT_RESERVE;


            adjustOwnerCount(view, sle, 1, j);

        }

        // update state count
        hook->setFieldU32(sfHookStateCount, stateCount);

    }

    // add new data to ledger
    auto newHookState = std::make_shared<SLE>(hookStateKeylet);
    view.insert(newHookState);
    newHookState->setFieldVL(sfHookStateData, data);
    newHookState->setFieldH256(sfHookStateKey, key);
    newHookState->setAccountID(sfAccount, hookResult.account);

    if (!oldHookState) {
        // Add the hook to the account's directory if it wasn't there already
        auto const page = dirAdd(
            view,
            hookResult.ownerDirKeylet,
            hookStateKeylet.key,
            false,
            describeOwnerDir(hookResult.account),
            j);

        JLOG(j.trace()) << "HookInfo[" << HR_ACC() << "]: "
            << "Create/update hook state: "
            << (page ? "success" : "failure");

        if (!page)
            return tecDIR_FULL;

        newHookState->setFieldU64(sfOwnerNode, *page);

    }

    return tesSUCCESS;
}

hook::HookResult
    hook::apply(
        ripple::uint256 hookSetTxnID, /* this is the txid of the sethook, used for caching (one day) */
        ripple::uint256 hookHash,     /* hash of the actual hook byte code, used for metadata */
        Blob hook, ApplyContext& applyCtx,
        const AccountID& account,     /* the account the hook is INSTALLED ON not necessarily the otxn account */
        bool callback = false)
{

    HookContext hookCtx =
    {
        .applyCtx = applyCtx,
        // we will return this context object (RVO / move constructed)
        .result = {
            .hookSetTxnID = hookSetTxnID,
            .hookHash = hookHash,
            .accountKeylet = keylet::account(account),
            .ownerDirKeylet = keylet::ownerDir(account),
            .hookKeylet = keylet::hook(account),
            .account = account,
            .otxnAccount = applyCtx.tx.getAccountID(sfAccount),
            .changedState =
                std::make_shared<std::map<ripple::uint256, std::pair<bool, ripple::Blob>>>(),
            .exitType = hook_api::ExitType::ROLLBACK, // default is to rollback unless hook calls accept()
            .exitReason = std::string(""),
            .exitCode = -1
        }
    };

    auto const& j = applyCtx.app.journal("View");

    SSVM::VM::Configure cfg;
    SSVM::VM::VM vm(cfg);
    HookModule env(hookCtx);
    vm.registerModule(env);

    std::vector<SSVM::ValVariant> params, results;
    params.push_back(0UL);

    JLOG(j.trace())
        << "HookInfo[" << HC_ACC() << "]: creating wasm instance";
    if (auto result =
            vm.runWasmFile(
                SSVM::Span<const uint8_t>(hook.data(), hook.size()), (callback ? "cbak" : "hook"), params))
        hookCtx.result.instructionCount = vm.getStatistics().getInstrCount();
    else
    {
        uint32_t ssvm_error = static_cast<uint32_t>(result.error());
        if (ssvm_error > 1)
        {
            JLOG(j.warn())
                << "HookError[" << HC_ACC() << "]: SSVM error " << ssvm_error;
            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            return hookCtx.result;
        }
    }

    JLOG(j.trace()) <<
        "HookInfo[" << HC_ACC() << "]: " <<
            ( hookCtx.result.exitType == hook_api::ExitType::ROLLBACK ? "ROLLBACK" : "ACCEPT" ) <<
        " RS: '" <<  hookCtx.result.exitReason.c_str() << "' RC: " << hookCtx.result.exitCode;

    // callback auto-commits on non-rollback
    if (callback)
    {
        // importantly the callback always removes the entry from the ltEMITTED structure
        uint8_t cclMode = hook::cclREMOVE;
        // we will only apply changes from the callback if the callback accepted
        if (hookCtx.result.exitType == hook_api::ExitType::ACCEPT)
            cclMode |= hook::cclAPPLY;
        commitChangesToLedger(hookCtx.result, applyCtx, cclMode);
    }

    return hookCtx.result;
}

/* If XRPLD is running with trace log level hooks may produce debugging output to the trace log
 * specifying both a string and an integer to output */
DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_num,
    uint32_t read_ptr, uint32_t read_len, int64_t number)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx on current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    RETURN_HOOK_TRACE(read_ptr, read_len, number);
}


DEFINE_HOOK_FUNCTION(
    int64_t,
    trace,
    uint32_t mread_ptr, uint32_t mread_len,
    uint32_t dread_ptr, uint32_t dread_len, uint32_t as_hex )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx on current stack
    if (NOT_IN_BOUNDS(mread_ptr, mread_len, memory_length) ||
        NOT_IN_BOUNDS(dread_ptr, dread_len, memory_length))
        return OUT_OF_BOUNDS;
    
    if (!j.trace())
        return 0;

    if (mread_len > 128)
        mread_len = 128;
    
    if (dread_len > 1024)
        dread_len = 1024;

    uint8_t output[2048];
    size_t out_len = 0;
    if (as_hex)
    {
        out_len = dread_len * 2;
        for (int i = 0; i < dread_len && i < memory_length; ++i)
        {
            unsigned char high = (memory[dread_ptr + i] >> 4) & 0xF;
            unsigned char low  = (memory[dread_ptr + i] & 0xF);
            high += ( high < 10U ? '0' : 'A' - 10 );
            low  += ( low  < 10U ? '0' : 'A' - 10 );
            output[i*2 + 0] = high;
            output[i*2 + 1] = low;
        }
    }
    else if (is_UTF16LE(memory + dread_ptr, dread_len))
    {
        out_len = dread_len / 2; //is_UTF16LE will only return true if read_len is even
        for (int i = 0; i < out_len; ++i)
            output[i] = memory[dread_ptr + i * 2];
    }
   
    RETURN_HOOK_TRACE(mread_ptr, mread_len, 
           std::string_view((const char*)output, out_len));
}


// zero pad on the left a key to bring it up to 32 bytes
std::optional<ripple::uint256>
inline
make_state_key(
        std::string_view source)
{

    size_t source_len = source.size();

    if (source_len > 32 || source_len < 1)
        return std::nullopt;

    unsigned char key_buffer[32];
    int i = 0;
    int pad = 32 - source_len;

    // zero pad on the left
    for (; i < pad; ++i)
        key_buffer[i] = 0;

    const char* data = source.data();

    for (; i < 32; ++i)
        key_buffer[i] = data[i - pad];

    return ripple::uint256::fromVoid(key_buffer);
}

// update or create a hook state object
// read_ptr = data to set, kread_ptr = key
// RH NOTE passing 0 size causes a delete operation which is as-intended
// RH TODO: check reserve
DEFINE_HOOK_FUNCTION(
    int64_t,
    state_set,
    uint32_t read_ptr, uint32_t read_len,
    uint32_t kread_ptr, uint32_t kread_len )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(kread_ptr, 32, memory_length))
        return OUT_OF_BOUNDS;

    if (read_ptr == 0 && read_len == 0)
    {
        // valid, this is a delete operation
    }
    else if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (kread_len > 32)
        return TOO_BIG;

    if (kread_len < 1)
        return TOO_SMALL;

    auto const sle = view.peek(hookCtx.result.hookKeylet);
    if (!sle)
        return INTERNAL_ERROR;

    uint32_t maxSize = sle->getFieldU32(sfHookStateDataMaxSize);
    if (read_len > maxSize)
        return TOO_BIG;

    auto const key =
        make_state_key( std::string_view { (const char*)(memory + kread_ptr), (size_t)kread_len } );

    (*hookCtx.result.changedState)[*key] =
        std::pair<bool, ripple::Blob> (true,
                {memory + read_ptr,  memory + read_ptr + read_len});

    return read_len;
}


void hook::commitChangesToLedger(
        hook::HookResult& hookResult,
        ripple::ApplyContext& applyCtx,
        uint8_t cclMode = 0b11U)
    /* Mode: (Bits)
     *  (MSB)      (LSB)
     * ------------------------
     * | cclRemove | cclApply |
     * ------------------------
     * | 1         | 1        |  Remove old ltEMITTED entry (where applicable) and apply state changes
     * | 0         | 1        |  Apply but don't Remove ltEMITTED entry
     * | 1         | 0        |  Remove but don't Apply (used when rollback on an emitted txn)
     * | 0         | 0        |  Invalid option
     * ------------------------
     */
{

    auto const& j = applyCtx.app.journal("View");
    if (cclMode == 0)
    {
        JLOG(j.warn()) <<
            "HookError[" << HR_ACC() << "]: commitChangesToLedger called with invalid mode (00)";
        return;
    }

    uint16_t change_count = 0;

    // write hook state changes, if we are allowed to
    if (cclMode & cclAPPLY)
    {
        // write all changes to state, if in "apply" mode
        for (const auto& cacheEntry : *(hookResult.changedState)) {
            bool is_modified = cacheEntry.second.first;
            const auto& key = cacheEntry.first;
            const auto& blob = cacheEntry.second.second;
            if (is_modified) {
                change_count++;
                // this entry isn't just cached, it was actually modified
                auto HSKeylet = keylet::hook_state(hookResult.account, key);
                auto slice = Slice(blob.data(), blob.size());
                setHookState(hookResult, applyCtx, HSKeylet, key, slice);
                // ^ should not fail... checks were done before map insert
            }
        }

    }

    // open views do not modify add/remove ledger entries
    if (applyCtx.view().open())
        return;

    //RH TODO: this seems hacky... and also maybe there's a way this cast might fail?
    ApplyViewImpl& avi = dynamic_cast<ApplyViewImpl&>(applyCtx.view());

    uint16_t exec_index = avi.nextHookExecutionIndex();
    uint16_t emission_count = 0;
    // apply emitted transactions to the ledger (by adding them to the emitted directory) if we are allowed to
    if (cclMode & cclAPPLY)
    {
        DBG_PRINTF("emitted txn count: %d\n", hookResult.emittedTxn.size());
        for (; hookResult.emittedTxn.size() > 0; hookResult.emittedTxn.pop())
        {
            auto& tpTrans = hookResult.emittedTxn.front();
            auto& id = tpTrans->getID();
            JLOG(j.trace())
                << "HookEmit[" << HR_ACC() << "]: " << id;

            std::shared_ptr<const ripple::STTx> ptr = tpTrans->getSTransaction();

            ripple::Serializer s;
            ptr->add(s);
            SerialIter sit(s.slice());

            auto emittedId = keylet::emitted(id);

            auto sleEmitted = applyCtx.view().peek(keylet::emitted(id));
            if (!sleEmitted)
            {
                ++emission_count;
                sleEmitted = std::make_shared<SLE>(emittedId);
                sleEmitted->emplace_back(
                    ripple::STObject(sit, sfEmittedTxn)
                );
                auto page = applyCtx.view().dirAppend(
                    keylet::emittedDir(),
                    emittedId,
                    [&](SLE::ref sle) {
                    // RH TODO: should something be here?
                    });

                if (page)
                {
                    (*sleEmitted)[sfOwnerNode] = *page;
                    applyCtx.view().insert(sleEmitted);
                }
                else
                {
                    JLOG(j.warn()) << "HookError[" << HR_ACC() << "]: " <<
                        "Emission Directory full when trying to insert " << id;
                    break;
                }
            }
        }
    }

    // remove this (activating) transaction from the emitted directory if we were instructed to
    if (cclMode & cclREMOVE)
    {
        do
        {
            auto const& tx = applyCtx.tx;
            if (!const_cast<ripple::STTx&>(tx).isFieldPresent(sfEmitDetails))
                break;

            auto key = keylet::emitted(tx.getTransactionID());

            auto const& sle = applyCtx.view().peek(key);

            if (!sle)
            {
                JLOG(j.warn())
                    << "HookError[" << HR_ACC() << "]: ccl tried to remove already removed emittedtxn";
                break;
            }

            if (!applyCtx.view().dirRemove(
                    keylet::emittedDir(),
                    sle->getFieldU64(sfOwnerNode),
                    key,
                    false))
            {
                JLOG(j.fatal())
                    << "HookError[" << HR_ACC() << "]: ccl tefBAD_LEDGER";
                break;
            }

            applyCtx.view().erase(sle);
        } while (0);
    }

    // add a metadata entry for this hook execution result
    STObject meta { sfHookExecution };
    meta.setFieldU8(sfHookResult, hookResult.exitType );
    meta.setFieldH256(sfHookHash, hookResult.hookSetTxnID);
    meta.setAccountID(sfHookAccount, hookResult.account);

    // RH NOTE: this is probably not necessary, a direct cast should always put the (negative) 1 bit at the MSB
    // however to ensure this is consistent across different arch/compilers it's done explicitly here.
    uint64_t unsigned_exit_code =
        ( hookResult.exitCode >= 0 ? hookResult.exitCode :
            0x8000000000000000ULL + (-1 * hookResult.exitCode ));

    meta.setFieldU64(sfHookReturnCode, unsigned_exit_code);
    meta.setFieldVL(sfHookReturnString, ripple::Slice{hookResult.exitReason.data(), hookResult.exitReason.size()});
    meta.setFieldU64(sfHookInstructionCount, hookResult.instructionCount);
    meta.setFieldU16(sfHookEmitCount, emission_count); // this will never wrap, hard limit
    meta.setFieldU16(sfHookExecutionIndex, exec_index );
    meta.setFieldU16(sfHookStateChangeCount, change_count );
    avi.addHookMetaData(std::move(meta));
}

/* Retrieve the state into write_ptr identified by the key in kread_ptr */
DEFINE_HOOK_FUNCTION(
    int64_t,
    state,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t kread_ptr, uint32_t kread_len )
{
    return  state_foreign(
                hookCtx, memoryCtx,
                write_ptr, write_len,
                kread_ptr, kread_len,
                0, 0);
}

/* This api actually serves both local and foreign state requests
 * feeding aread_ptr = 0 and aread_len = 0 will cause it to read local */
DEFINE_HOOK_FUNCTION(
    int64_t,
    state_foreign,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t kread_ptr, uint32_t kread_len,
    uint32_t aread_ptr, uint32_t aread_len )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    bool is_foreign = false;
    if (aread_ptr == 0)
    {
        // valid arguments, local state
    } else if (aread_ptr > 0)
    {
        // valid arguments, foreign state
        is_foreign = true;
    } else return INVALID_ARGUMENT;

    if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length) ||
        NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
        NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (kread_len > 32)
        return TOO_BIG;

    if (is_foreign && aread_len != 20)
        return INVALID_ACCOUNT;


    auto const key =
        make_state_key( std::string_view { (const char*)(memory + kread_ptr), (size_t)kread_len } );

    if (!key)
        return INVALID_ARGUMENT;

    // first check if the requested state was previously cached this session
    if (!is_foreign) // we only cache local
    {
        const auto& cacheEntry = hookCtx.result.changedState->find(*key);
        if (cacheEntry != hookCtx.result.changedState->end())
        {
            if (write_ptr == 0)
                return data_as_int64(cacheEntry->second.second.data(), cacheEntry->second.second.size());

            if (cacheEntry->second.second.size() > write_len)
                return TOO_SMALL;

            WRITE_WASM_MEMORY_AND_RETURN(
                write_ptr, write_len,
                cacheEntry->second.second.data(), cacheEntry->second.second.size(),
                memory, memory_length);
        }
    }

    // cache miss look it up
    auto const sle = view.peek(hookCtx.result.hookKeylet);
    if (!sle)
        return INTERNAL_ERROR;

    auto hsSLE = view.peek(keylet::hook_state(
                (is_foreign ? AccountID::fromVoid(memory + aread_ptr) : hookCtx.result.account), *key));
    if (!hsSLE)
        return DOESNT_EXIST;

    Blob b = hsSLE->getFieldVL(sfHookStateData);

    // it exists add it to cache and return it

    if (!is_foreign)
        hookCtx.result.changedState->emplace(*key, std::pair<bool, ripple::Blob>(false, b));

    if (write_ptr == 0)
        return data_as_int64(b.data(), b.size());

    if (b.size() > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        b.data(), b.size(),
        memory, memory_length);
}


// Cause the originating transaction to go through, save state changes and emit emitted tx, exit hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    accept,
    uint32_t read_ptr, uint32_t read_len, int64_t error_code )
{
    HOOK_SETUP();
    HOOK_EXIT(read_ptr, read_len, error_code, hook_api::ExitType::ACCEPT);
}

// Cause the originating transaction to be rejected, discard state changes and discard emitted tx, exit hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    rollback,
    uint32_t read_ptr, uint32_t read_len, int64_t error_code )
{
    HOOK_SETUP();
    HOOK_EXIT(read_ptr, read_len, error_code, hook_api::ExitType::ROLLBACK);
}


// Write the TxnID of the originating transaction into the write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_id,
    uint32_t write_ptr, uint32_t write_len )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    auto const& txID = applyCtx.tx.getTransactionID();

    if (txID.size() > write_len)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, txID.size(), memory_length))
        return OUT_OF_BOUNDS;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, txID.size(),
        txID.data(), txID.size(),
        memory, memory_length);
}

// Return the tt (Transaction Type) numeric code of the originating transaction
DEFINE_HOOK_FUNCNARG(
        int64_t,
        otxn_type )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    return applyCtx.tx.getTxnType();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_slot,
    uint32_t slot_into )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (slot_into > hook_api::max_slots)
        return INVALID_ARGUMENT;

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    if (slot_into == 0)
        slot_into = get_free_slot(hookCtx);

    auto const& st_tx =
        std::make_shared<ripple::STObject>(const_cast<ripple::STTx&>(applyCtx.tx).downcast<ripple::STObject>());

    auto const& txID = applyCtx.tx.getTransactionID();

    hookCtx.slot.emplace( std::pair<int, hook::SlotEntry> { slot_into, hook::SlotEntry {
            .id = std::vector<uint8_t>{ txID.data(), txID.data() + txID.size() },
            .storage = st_tx,
            .entry = 0
    }});
    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;

}
// Return the burden of the originating transaction... this will be 1 unless the originating transaction
// was itself an emitted transaction from a previous hook invocation
DEFINE_HOOK_FUNCNARG(
        int64_t,
        otxn_burden)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (hookCtx.burden)
        return hookCtx.burden;

    auto const& tx = applyCtx.tx;
    if (!tx.isFieldPresent(sfEmitDetails))
        return 1; // burden is always 1 if the tx wasn't a emit

    auto const& pd = const_cast<ripple::STTx&>(tx).getField(sfEmitDetails).downcast<STObject>();

    if (!pd.isFieldPresent(sfEmitBurden)) {
        JLOG(j.warn())
            << "HookError[" << HC_ACC() << "]: found sfEmitDetails but sfEmitBurden was not present";
        return 1;
    }

    uint64_t burden = pd.getFieldU64(sfEmitBurden);
    burden &= ((1ULL << 63)-1); // wipe out the two high bits just in case somehow they are set
    hookCtx.burden = burden;
    return (int64_t)(burden);
}

// Return the generation of the originating transaction... this will be 1 unless the originating transaction
// was itself an emitted transaction from a previous hook invocation
DEFINE_HOOK_FUNCNARG(
        int64_t,
        otxn_generation)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    // cache the result as it will not change for this hook execution
    if (hookCtx.generation)
        return hookCtx.generation;

    auto const& tx = applyCtx.tx;
    if (!tx.isFieldPresent(sfEmitDetails))
        return 1; // generation is always 1 if the tx wasn't a emit

    auto const& pd = const_cast<ripple::STTx&>(tx).getField(sfEmitDetails).downcast<STObject>();

    if (!pd.isFieldPresent(sfEmitGeneration)) {
        JLOG(j.warn())
            << "HookError[" << HC_ACC() << "]: found sfEmitDetails but sfEmitGeneration was not present";
        return 1;
    }

    hookCtx.generation = pd.getFieldU32(sfEmitGeneration);
    // this overflow will never happen in the life of the ledger but deal with it anyway
    if (hookCtx.generation + 1 > hookCtx.generation)
        hookCtx.generation++;

    return hookCtx.generation;
}

// Return the generation of a hypothetically emitted transaction from this hook
DEFINE_HOOK_FUNCNARG(
        int64_t,
        etxn_generation)
{
    return otxn_generation(hookCtx, memoryCtx) + 1;
}


// Return the current ledger sequence number
DEFINE_HOOK_FUNCNARG(
        int64_t,
        ledger_seq)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    return applyCtx.app.getLedgerMaster().getValidLedgerIndex() + 1;
}


// Dump a field in 'full text' form into the hook's memory
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_field_txt,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t field_id )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const& tx = applyCtx.tx;

    SField const& fieldType = ripple::SField::getField( field_id );

    if (fieldType == sfInvalid)
        return INVALID_FIELD;

    if (!tx.isFieldPresent(fieldType))
        return DOESNT_EXIST;

    auto const& field = const_cast<ripple::STTx&>(tx).getField(fieldType);

    std::string out = field.getText();

    if (out.size() > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        out.data(), out.size(),
        memory, memory_length);

}

// Dump a field from the originating transaction into the hook's memory
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_field,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t field_id )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (write_ptr != 0 && NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const& tx = applyCtx.tx;

    SField const& fieldType = ripple::SField::getField( field_id );

    if (fieldType == sfInvalid)
        return INVALID_FIELD;

    if (!tx.isFieldPresent(fieldType))
        return DOESNT_EXIST;

    auto const& field = const_cast<ripple::STTx&>(tx).getField(fieldType);

    bool is_account = field.getSType() == STI_ACCOUNT; //RH TODO improve this hack

    Serializer s;
    field.add(s);

    if (write_ptr == 0)
        return data_as_int64(s.getDataPtr(), s.getDataLength());

    if (s.getDataLength() - (is_account ? 1 : 0) > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        (unsigned char*)(s.getDataPtr()) + (is_account ? 1 : 0), s.getDataLength() - (is_account ? 1 : 0),
        memory, memory_length);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t slot_no )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (!(write_ptr == 0 && write_len == 0) &&
        NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_ptr != 0 && write_len == 0)
        return TOO_SMALL;

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    Serializer s;
    hookCtx.slot[slot_no].entry->add(s);

    if (write_ptr == 0)
        return data_as_int64(s.getDataPtr(), s.getDataLength());

    bool is_account = hookCtx.slot[slot_no].entry->getSType() == STI_ACCOUNT; //RH TODO improve this hack

    if (s.getDataLength() - (is_account ? 1 : 0) > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        (unsigned char*)(s.getDataPtr()) + (is_account ? 1 : 0), s.getDataLength() - (is_account ? 1 : 0),
        memory, memory_length);

}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_clear,
    uint32_t slot_no )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    hookCtx.slot.erase(slot_no);
    hookCtx.slot_free.push(slot_no);

    return 1;
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_count,
    uint32_t slot_no )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry->getSType() != STI_ARRAY)
        return NOT_AN_ARRAY;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    return hookCtx.slot[slot_no].entry->downcast<ripple::STArray>().size();
}



DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_id,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t slot_no )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    auto& e = hookCtx.slot[slot_no].id;

    if (write_len < e.size())
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        e.data(), e.size(),
        memory, memory_length);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_set,
    uint32_t read_ptr, uint32_t read_len,   // readptr is a keylet
    int32_t slot_into /* providing 0 allocates a slot to you */ )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if ((read_len != 32 && read_len != 34) || slot_into < 0 || slot_into > hook_api::max_slots)
        return INVALID_ARGUMENT;

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    std::vector<uint8_t> slot_key { memory + read_ptr, memory + read_ptr + read_len };
    std::optional<std::shared_ptr<const ripple::STObject>> slot_value = std::nullopt;

    if (read_len == 34)
    {
        std::optional<ripple::Keylet> kl = unserialize_keylet(memory + read_ptr, read_len);
        if (!kl)
            return DOESNT_EXIST;

        auto sle = applyCtx.view().peek(*kl);
        if (!sle)
            return DOESNT_EXIST;

        slot_value = sle;
    }
    else if (read_len == 32)
    {

        uint256 hash;
        if (!hash.SetHexExact((const char*)(memory + read_ptr)))
            return INVALID_ARGUMENT;

        ripple::error_code_i ec { ripple::error_code_i::rpcUNKNOWN };
        std::shared_ptr<ripple::Transaction> hTx = applyCtx.app.getMasterTransaction().fetch(hash, ec);
        if (!hTx)
            return DOESNT_EXIST;

        slot_value = hTx->getSTransaction();
    }
    else
        return DOESNT_EXIST;

    if (!slot_value.has_value())
        return DOESNT_EXIST;

    if (slot_into == 0)
        slot_into = get_free_slot(hookCtx);


    hookCtx.slot.emplace( std::pair<int, hook::SlotEntry> { slot_into, hook::SlotEntry {
            .id = slot_key,
            .storage = *slot_value,
            .entry = 0
    }});
    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_size,
    uint32_t slot_no )
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    //RH TODO: this is a very expensive way of computing size, fix it
    Serializer s;
    hookCtx.slot[slot_no].entry->add(s);
    return s.getDataLength();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_subarray,
    uint32_t parent_slot, uint32_t array_id, uint32_t new_slot )
{
    if (hookCtx.slot.find(parent_slot) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[parent_slot].entry->getSType() != STI_ARRAY)
        return NOT_AN_ARRAY;

    if (hookCtx.slot[parent_slot].entry == 0)
        return INTERNAL_ERROR;

    if (new_slot == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    bool copied = false;
    try
    {
        ripple::STArray& parent_obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[parent_slot].entry).downcast<ripple::STArray>();

        if (parent_obj.size() <= array_id)
        return DOESNT_EXIST;
        new_slot = ( new_slot == 0 ? get_free_slot(hookCtx) : new_slot );

        // copy
        if (new_slot != parent_slot)
        {
            copied = true;
            hookCtx.slot[new_slot] = hookCtx.slot[parent_slot];
        }
        hookCtx.slot[new_slot].entry = &(parent_obj[array_id]);
        return new_slot;
    }
    catch (const std::bad_cast& e)
    {
        if (copied)
        {
            hookCtx.slot.erase(new_slot);
            hookCtx.slot_free.push(new_slot);
        }
        return NOT_AN_ARRAY;
    }
}


DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_subfield,
    uint32_t parent_slot, uint32_t field_id, uint32_t new_slot)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(parent_slot) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (new_slot == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    SField const& fieldCode = ripple::SField::getField( field_id );

    if (fieldCode == sfInvalid)
        return INVALID_FIELD;

    bool copied = false;

    try
    {
        ripple::STObject& parent_obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[parent_slot].entry).downcast<ripple::STObject>();

        if (!parent_obj.isFieldPresent(fieldCode))
            return DOESNT_EXIST;

        new_slot = ( new_slot == 0 ? get_free_slot(hookCtx) : new_slot );

        // copy
        if (new_slot != parent_slot)
        {
            copied = true;
            hookCtx.slot[new_slot] = hookCtx.slot[parent_slot];
        }

        hookCtx.slot[new_slot].entry = &(parent_obj.getField(fieldCode));
        return new_slot;
    }
    catch (const std::bad_cast& e)
    {
        if (copied)
        {
            hookCtx.slot.erase(new_slot);
            hookCtx.slot_free.push(new_slot);
        }
        return NOT_AN_OBJECT;
    }

}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_type,
    uint32_t slot_no, uint32_t flags )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;
    try
    {
        ripple::STBase& obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry);//.downcast<ripple::STBase>();
            if (flags == 0)
                return obj.getFName().fieldCode;

            // this flag is for use with an amount field to determine if the amount is native (xrp)
            if (flags == 1)
            {
                if (obj.getSType() != STI_AMOUNT)
                    return NOT_AN_AMOUNT;
                return
                    const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry).downcast<ripple::STAmount>().native();
            }

            return INVALID_ARGUMENT;
    }
    catch (const std::bad_cast& e)
    {
        return INTERNAL_ERROR;
    }
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_float,
    uint32_t slot_no )
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    try
    {
        ripple::STAmount& st_amt =
            const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry).downcast<ripple::STAmount>();
        if (st_amt.native())
        {
            ripple::XRPAmount amt = st_amt.xrp();
            int64_t drops = amt.drops();
            int32_t exp = -6;
            // normalize
            return hook_float::float_set(exp, drops);
        }
        else
        {
            ripple::IOUAmount amt = st_amt.iou();
            return make_float(amt);
        }
    }
    catch (const std::bad_cast& e)
    {
        return NOT_AN_AMOUNT;
    }

}

DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_slot,
    uint32_t read_ptr, uint32_t read_len,
    uint32_t slot_no )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;
    
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    uint8_t* id = hookCtx.slot[slot_no].id.data();
    size_t id_size = hookCtx.slot[slot_no].id.size();
    uint8_t output[64];
    if (id_size > 32) id_size = 32;
    for (int i = 0; i < id_size; ++i)
    {
        unsigned char high = (id[i] >> 4) & 0xF;
        unsigned char low  = (id[i] & 0xF);
        high += ( high < 10U ? '0' : 'A' - 10 );
        low  += ( low  < 10U ? '0' : 'A' - 10 );
        output[i*2 + 0] = high;
        output[i*2 + 1] = low;
    }

    RETURN_HOOK_TRACE(read_ptr, read_len, 
            "Slot " << slot_no << " - "
            << std::string_view((const char*)output, (size_t)(id_size*2)));
}


DEFINE_HOOK_FUNCTION(
    int64_t,
    util_keylet,
    uint32_t write_ptr, uint32_t write_len, uint32_t keylet_type,
    uint32_t a,         uint32_t b,         uint32_t c,
    uint32_t d,         uint32_t e,         uint32_t f )
{
    HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < 34)
        return TOO_SMALL;

    if (keylet_type < 1 || keylet_type > 21)
        return INVALID_ARGUMENT;

    try
    {
        switch (keylet_type)
        {

            // keylets that take a keylet and an 8 byte uint
            case keylet_code::QUALITY:
            {
                if (a == 0 || b == 0 || c == 0 || d == 0)
                    return INVALID_ARGUMENT;
                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (read_len != 34)
                    return INVALID_ARGUMENT;

                std::optional<ripple::Keylet> kl = unserialize_keylet(memory + read_ptr, read_len);
                if (!kl)
                    return NO_SUCH_KEYLET;

                uint64_t arg = (((uint64_t)c)<<32U)+((uint64_t)d);

                ripple::Keylet kl_out =
                    ripple::keylet::quality(*kl, arg);

                return serialize_keylet(kl_out, memory, write_ptr, write_len);
            }

            // keylets that take a 32 byte uint
            case keylet_code::CHILD:
            case keylet_code::EMITTED:
            case keylet_code::UNCHECKED:
            {
                if (a == 0 || b == 0)
                   return INVALID_ARGUMENT;

                if (c != 0 || d != 0 || e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (read_len != 32)
                    return INVALID_ARGUMENT;

                base_uint<256> id = ripple::base_uint<256>::fromVoid(memory + read_ptr);

                ripple::Keylet kl =
                    keylet_type == keylet_code::CHILD        ? ripple::keylet::child(id)            :
                    keylet_type == keylet_code::EMITTED      ? ripple::keylet::emitted(id)          :
                    ripple::keylet::unchecked(id);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take a 20 byte account id
            case keylet_code::OWNER_DIR:
            case keylet_code::SIGNERS:
            case keylet_code::ACCOUNT:
            case keylet_code::HOOK:
            {
                if (a == 0 || b == 0)
                   return INVALID_ARGUMENT;

                if (c != 0 || d != 0 || e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (read_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID id =
                    ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + read_ptr);

                ripple::Keylet kl =
                    keylet_type == keylet_code::HOOK        ? ripple::keylet::hook(id)      :
                    keylet_type == keylet_code::SIGNERS     ? ripple::keylet::signers(id)   :
                    keylet_type == keylet_code::OWNER_DIR   ? ripple::keylet::ownerDir(id)  :
                    ripple::keylet::account(id);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take 20 byte account id, and 4 byte uint
            case keylet_code::OFFER:
            case keylet_code::CHECK:
            case keylet_code::ESCROW:
            {
                if (a == 0 || b == 0 || c == 0)
                    return INVALID_ARGUMENT;
                if (d != 0 || e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (read_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID id =
                    ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + read_ptr);

                ripple::Keylet kl =
                    keylet_type == keylet_code::CHECK       ? ripple::keylet::check(id, c)      :
                    keylet_type == keylet_code::ESCROW      ? ripple::keylet::escrow(id, c)     :
                    ripple::keylet::offer(id, c);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take a 32 byte uint and an 8byte uint64
            case keylet_code::PAGE:
            {
                if (a == 0 || b == 0 || c == 0 || d == 0)
                   return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                uint32_t kread_ptr = a, kread_len = b;

                if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (b != 32)
                    return INVALID_ARGUMENT;

                uint64_t index = (((uint64_t)c)<<32U) + ((uint64_t)d);
                ripple::Keylet kl = ripple::keylet::page(ripple::base_uint<256>::fromVoid(memory + a), index);
                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take both a 20 byte account id and a 32 byte uint
            case keylet_code::HOOK_STATE:
            {
                if (a == 0 || b == 0 || c == 0 || d == 0)
                   return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b, kread_ptr = c, kread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (aread_len != 20 || kread_len != 32)
                    return INVALID_ARGUMENT;

                ripple::Keylet kl =
                    ripple::keylet::hook_state(
                            ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + aread_ptr),
                            ripple::base_uint<256>::fromVoid(memory + kread_ptr));

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }


            // skip is overloaded, has a single, optional 4 byte argument
            case keylet_code::SKIP:
            {
                if (c != 0 || d != 0 || e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                ripple::Keylet kl =
                    (b == 0 ? ripple::keylet::skip() :
                    ripple::keylet::skip(a));

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // no arguments
            case keylet_code::AMENDMENTS:
            case keylet_code::FEES:
            case keylet_code::NEGATIVE_UNL:
            case keylet_code::EMITTED_DIR:
            {
                if (a != 0 || b != 0 || c != 0 || d != 0 || e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                ripple::Keylet kl =
                    keylet_type == keylet_code::AMENDMENTS   ? ripple::keylet::amendments()       :
                    keylet_type == keylet_code::FEES         ? ripple::keylet::fees()             :
                    keylet_type == keylet_code::NEGATIVE_UNL ? ripple::keylet::negativeUNL()      :
                    ripple::keylet::emittedDir();

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            case keylet_code::LINE:
            {
                if (a == 0 || b == 0 || c == 0 || d == 0 || e == 0 || f == 0)
                   return INVALID_ARGUMENT;

                uint32_t hi_ptr = a, hi_len = b, lo_ptr = c, lo_len = d, cu_ptr = e, cu_len = f;

                if (NOT_IN_BOUNDS(hi_ptr, hi_len, memory_length) ||
                    NOT_IN_BOUNDS(lo_ptr, lo_len, memory_length) ||
                    NOT_IN_BOUNDS(cu_ptr, cu_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (hi_len != 20 || lo_len != 20 || cu_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID a0 = ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + hi_ptr);
                ripple::AccountID a1 = ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + lo_ptr);
                ripple::Currency  cu = ripple::base_uint<160, ripple::detail::CurrencyTag>::fromVoid(memory + cu_ptr);

                ripple::Keylet kl =
                    ripple::keylet::line(a0, a1, cu);
                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take two 20 byte account ids
            case keylet_code::DEPOSIT_PREAUTH:
            {
                if (a == 0 || b == 0 || c == 0 || d == 0)
                   return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                   return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b;
                uint32_t bread_ptr = c, bread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(bread_ptr, bread_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (aread_len != 20 || bread_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID aid =
                    ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + aread_ptr);
                ripple::AccountID bid =
                    ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + bread_ptr);

                ripple::Keylet kl =
                    ripple::keylet::depositPreauth(aid, bid);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take two 20 byte account ids and a 4 byte uint
            case keylet_code::PAYCHAN:
            {
                if (a == 0 || b == 0 || c == 0 || d == 0 || e == 0)
                   return INVALID_ARGUMENT;

                if (f != 0)
                   return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b;
                uint32_t bread_ptr = c, bread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(bread_ptr, bread_len, memory_length))
                   return OUT_OF_BOUNDS;

                if (aread_len != 20 || bread_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID aid =
                    ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + aread_ptr);
                ripple::AccountID bid =
                    ripple::base_uint<160, ripple::detail::AccountIDTag>::fromVoid(memory + bread_ptr);

                ripple::Keylet kl =
                    ripple::keylet::payChan(aid, bid, e);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

        }
    }
    catch (std::exception& e)
    {
        JLOG(j.warn())
            << "HookError[" << HC_ACC() << "]: Keylet exception " << e.what();
        return INTERNAL_ERROR;
    }

    return NO_SUCH_KEYLET;
}


/* Emit a transaction from this hook. Transaction must be in STObject form, fully formed and valid.
 * XRPLD does not modify transactions it only checks them for validity. */
DEFINE_HOOK_FUNCTION(
    int64_t,
    emit,
    uint32_t read_ptr, uint32_t read_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    auto& app = hookCtx.applyCtx.app;

    if (hookCtx.expected_etxn_count < 0)
        return PREREQUISITE_NOT_MET;

    if (hookCtx.result.emittedTxn.size() >= hookCtx.expected_etxn_count)
        return TOO_MANY_EMITTED_TXN;

    ripple::Blob blob{memory + read_ptr, memory + read_ptr + read_len};

    DBG_PRINTF("hook is emitting tx:-----\n");
    for (unsigned char c: blob)
        DBG_PRINTF("%02X", c);
    DBG_PRINTF("\n--------\n");

    std::shared_ptr<STTx const> stpTrans;
    try
    {
        stpTrans = std::make_shared<STTx const>(SerialIter { memory + read_ptr, read_len });
    }
    catch (std::exception& e)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: Failed " << e.what() << "\n";
        return EMISSION_FAILURE;
    }

    // check the emitted txn is valid
    /* Emitted TXN rules
     * 1. Sequence: 0
     * 2. PubSigningKey: 000000000000000
     * 3. sfEmitDetails present and valid
     * 4. No sfSignature
     * 5. LastLedgerSeq > current ledger, > firstledgerseq
     * 6. FirstLedgerSeq > current ledger
     * 7. Fee must be correctly high
     */

    // rule 1: sfSequence must be present and 0
    if (!stpTrans->isFieldPresent(sfSequence) || stpTrans->getFieldU32(sfSequence) != 0)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfSequence missing or non-zero";
        return EMISSION_FAILURE;
    }

    // rule 2: sfSigningPubKey must be present and 00...00
    if (!stpTrans->isFieldPresent(sfSigningPubKey))
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfSigningPubKey missing";
        return EMISSION_FAILURE;
    }

    auto const pk = stpTrans->getSigningPubKey();
    if (pk.size() != 33 && pk.size() != 0)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfSigningPubKey present but wrong size"
            << " expecting 33 bytes";
        return EMISSION_FAILURE;
    }

    for (int i = 0; i < pk.size(); ++i)
    if (pk[i] != 0)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfSigningPubKey present but non-zero.";
        return EMISSION_FAILURE;
    }

    // rule 3: sfEmitDetails must be present and valid
    if (!stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitDetails missing.";
        return EMISSION_FAILURE;
    }

    auto const& emitDetails =
        const_cast<ripple::STTx&>(*stpTrans).getField(sfEmitDetails).downcast<STObject>();

    if (!emitDetails.isFieldPresent(sfEmitGeneration) ||
        !emitDetails.isFieldPresent(sfEmitBurden) ||
        !emitDetails.isFieldPresent(sfEmitParentTxnID) ||
        !emitDetails.isFieldPresent(sfEmitNonce) ||
        !emitDetails.isFieldPresent(sfEmitCallback))
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitDetails malformed.";
        return EMISSION_FAILURE;
    }

    uint32_t gen = emitDetails.getFieldU32(sfEmitGeneration);
    uint64_t bur = emitDetails.getFieldU64(sfEmitBurden);
    ripple::uint256 pTxnID = emitDetails.getFieldH256(sfEmitParentTxnID);
    ripple::uint256 nonce = emitDetails.getFieldH256(sfEmitNonce);
    auto callback = emitDetails.getAccountID(sfEmitCallback);

    uint32_t gen_proper = etxn_generation(hookCtx, memoryCtx);

    if (gen != gen_proper)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitGeneration provided in EmitDetails "
            << "not correct (" << gen << ") "
            << "should be " << gen_proper;
        return EMISSION_FAILURE;
    }

    uint64_t bur_proper = etxn_burden(hookCtx, memoryCtx);
    if (bur != bur_proper)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitBurden provided in EmitDetails "
            << "was not correct (" << bur << ") "
            << "should be " << bur_proper;
        return EMISSION_FAILURE;
    }

    if (pTxnID != applyCtx.tx.getTransactionID())
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitParentTxnID provided in EmitDetails "
            << "was not correct";
        return EMISSION_FAILURE;
    }

    if (hookCtx.nonce_used.find(nonce) == hookCtx.nonce_used.end())
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitNonce provided in EmitDetails "
            << "was not generated by nonce api";
        return EMISSION_FAILURE;
    }

    if (callback != hookCtx.result.account)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfEmitCallback account must be the account "
            << "of the emitting hook";
        return EMISSION_FAILURE;
    }

    // rule 4: sfSignature must be absent
    if (stpTrans->isFieldPresent(sfSignature))
    {
        JLOG(j.trace()) <<
            "HookEmit[" << HC_ACC() << "]: sfSignature is present but should not be";
        return EMISSION_FAILURE;
    }

    // rule 5: LastLedgerSeq must be present and after current ledger
    // RH TODO: limit lastledgerseq, is this needed?

    uint32_t tx_lls = stpTrans->getFieldU32(sfLastLedgerSequence);
    uint32_t ledgerSeq = applyCtx.app.getLedgerMaster().getValidLedgerIndex() + 1;
    if (!stpTrans->isFieldPresent(sfLastLedgerSequence) || tx_lls < ledgerSeq + 1)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfLastLedgerSequence missing or invalid";
        return EMISSION_FAILURE;
    }

    // rule 6
    if (!stpTrans->isFieldPresent(sfFirstLedgerSequence) ||
            stpTrans->getFieldU32(sfFirstLedgerSequence) > tx_lls)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: sfFirstLedgerSequence must be present and "
            << ">= LastLedgerSequence";
        return EMISSION_FAILURE;
    }

    // rule 7 check the emitted txn pays the appropriate fee
    if (hookCtx.fee_base == 0)
        hookCtx.fee_base = etxn_fee_base(hookCtx, memoryCtx, read_len);

    int64_t minfee = hookCtx.fee_base * hook_api::drops_per_byte * read_len;
    if (minfee < 0 || hookCtx.fee_base < 0)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: Fee could not be calculated";
        return EMISSION_FAILURE;
    }

    if (!stpTrans->isFieldPresent(sfFee))
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: Fee missing from emitted tx";
        return EMISSION_FAILURE;
    }

    int64_t fee = stpTrans->getFieldAmount(sfFee).xrp().drops();
    if (fee < minfee)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: Fee on emitted txn is less than the minimum required fee";
        return EMISSION_FAILURE;
    }

    std::string reason;
    auto tpTrans = std::make_shared<Transaction>(stpTrans, reason, app);
    if (tpTrans->getStatus() != NEW)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC() << "]: tpTrans->getStatus() != NEW";
        return EMISSION_FAILURE;
    }

    hookCtx.result.emittedTxn.push(tpTrans);
    return read_len;
}

// When implemented will return the hash of the current hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_hash,
    uint32_t write_ptr, uint32_t write_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        hookCtx.result.hookHash.data(), 32,
        memory, memory_length);
}

// Write the account id that the running hook is installed on into write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_account,
    uint32_t write_ptr, uint32_t ptr_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(write_ptr, 20, memory_length))
        return OUT_OF_BOUNDS;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 20,
        hookCtx.result.account.data(), 20,
        memory, memory_length);
}

// Deterministic nonces (can be called multiple times)
// Writes nonce into the write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    nonce,
    uint32_t write_ptr, uint32_t write_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx, view on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (hookCtx.nonce_counter > hook_api::max_nonce)
        return TOO_MANY_NONCES;

    auto hash = ripple::sha512Half(
            ripple::HashPrefix::emitTxnNonce,
//            view.info().seq,
            applyCtx.tx.getTransactionID(),
            hookCtx.nonce_counter++,
            hookCtx.result.account
    );

    hookCtx.nonce_used[hash] = true;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32,
        hash.data(), 32,
        memory, memory_length);

    return 32;
}

// Reserve one or more transactions for emission from the running hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_reserve,
    uint32_t count )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (hookCtx.expected_etxn_count > -1)
        return ALREADY_SET;

    if (count > hook_api::max_emit)
        return TOO_BIG;

    hookCtx.expected_etxn_count = count;

    return count;
}

// Compute the burden of an emitted transaction based on a number of factors
DEFINE_HOOK_FUNCNARG(
    int64_t,
    etxn_burden)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint64_t last_burden = (uint64_t)otxn_burden(hookCtx, memoryCtx); // always non-negative so cast is safe

    uint64_t burden = last_burden * hookCtx.expected_etxn_count;
    if (burden < last_burden) // this overflow will never happen but handle it anyway
        return FEE_TOO_LARGE;

    return burden;
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    util_sha512h,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t read_ptr, uint32_t read_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx, view on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto hash = ripple::sha512Half(
       ripple::Slice { memory + read_ptr, read_len }
    );

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32,
        hash.data(), 32,
        memory, memory_length);
}


// RH NOTE this is a light-weight stobject parsing function for drilling into a provided serialzied object
// however it could probably be replaced by an existing class or routine or set of routines in XRPLD
// Returns object length including header bytes (and footer bytes in the event of array or object)
// negative indicates error
// -1 = unexpected end of bytes
// -2 = unknown type (detected early)
// -3 = unknown type (end of function)
// -4 = excessive stobject nesting
// -5 = excessively large array or object
inline int32_t get_stobject_length (
    unsigned char* start,   // in - begin iterator
    unsigned char* maxptr,  // in - end iterator
    int& type,              // out - populated by serialized type code
    int& field,             // out - populated by serialized field code
    int& payload_start,     // out - the start of actual payload data for this type
    int& payload_length,    // out - the length of actual payload data for this type
    int recursion_depth = 0)   // used internally
{
    if (recursion_depth > 10)
        return -4;

    unsigned char* end = maxptr;
    unsigned char* upto = start;
    int high = *upto >> 4;
    int low = *upto & 0xF;

    upto++; if (upto >= end) return -1;
    if (high > 0 && low > 0)
    {
        // common type common field
        type = high;
        field = low;
    } else if (high > 0) {
        // common type, uncommon field
        type = high;
        field = *upto++;
    } else if (low > 0) {
        // common field, uncommon type
        field = low;
        type = *upto++;
    } else {
        // uncommon type and field
        type = *upto++;
        if (upto >= end) return -1;
        field = *upto++;
    }

    DBG_PRINTF("%d get_st_object found field %d type %d\n", recursion_depth, field, type);

    if (upto >= end) return -1;

    // RH TODO: link this to rippled's internal STObject constants
    // E.g.:
    /*
    int field_code = (safe_cast<int>(type) << 16) | field;
    auto const& fieldObj = ripple::SField::getField;
    */

    if (type < 1 || type > 19 || ( type >= 9 && type <= 13))
        return -2;

    bool is_vl = (type == 8 /*ACCID*/ || type == 7 || type == 18 || type == 19);


    int length = -1;
    if (is_vl)
    {
        length = *upto++;
        if (upto >= end)
            return -1;

        if (length < 193)
        {
            // do nothing
        } else if (length > 192 && length < 241)
        {
            length -= 193;
            length *= 256;
            length += *upto++ + 193; if (upto > end) return -1;
        } else {
            int b2 = *upto++; if (upto >= end) return -1;
            length -= 241;
            length *= 65536;
            length += 12481 + (b2 * 256) + *upto++; if (upto >= end) return -1;
        }
    } else if ((type >= 1 && type <= 5) || type == 16 || type == 17 )
    {
        length =    (type ==  1 ?  2 :
                    (type ==  2 ?  4 :
                    (type ==  3 ?  8 :
                    (type ==  4 ? 16 :
                    (type ==  5 ? 32 :
                    (type == 16 ?  1 :
                    (type == 17 ? 20 : -1 )))))));

    } else if (type == 6) /* AMOUNT */
    {
        length =  (*upto >> 6 == 1) ? 8 : 48;
        if (upto >= end) return -1;
    }

    if (length > -1)
    {
        payload_start = upto - start;
        payload_length = length;
        DBG_PRINTF("%d get_stobject_length field: %d Type: %d VL: %s Len: %d Payload_Start: %d Payload_Len: %d\n",
            recursion_depth, field, type, (is_vl ? "yes": "no"), length, payload_start, payload_length);
        return length + (upto - start);
    }

    if (type == 15 || type == 14) /* Object / Array */
    {
       payload_start = upto - start;

       for(int i = 0; i < 1024; ++i)
       {
            int subfield = -1, subtype = -1, payload_start_ = -1, payload_length_ = -1;
            int32_t sublength = get_stobject_length(
                    upto, end, subtype, subfield, payload_start_, payload_length_, recursion_depth + 1);
            DBG_PRINTF("%d get_stobject_length i %d %d-%d, upto %d sublength %d\n", recursion_depth, i,
                    subtype, subfield, upto - start, sublength);
            if (sublength < 0)
                return -1;
            upto += sublength;
            if (upto >= end)
                return -1;

            if ((*upto == 0xE1U && type == 0xEU) ||
                (*upto == 0xF1U && type == 0xFU))
            {
                payload_length = upto - start - payload_start;
                upto++;
                return (upto - start);
            }
       }
       return -5;
    }

    return -3;

}

// Given an serialized object in memory locate and return the offset and length of the payload of a subfield of that
// object. Arrays are returned fully formed. If successful returns offset and length joined as int64_t.
// Use SUB_OFFSET and SUB_LENGTH to extract.
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_subfield,
    uint32_t read_ptr, uint32_t read_len, uint32_t field_id )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 1)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + read_len;

    DBG_PRINTF("sto_subfield called, looking for field %u type %u\n", field_id & 0xFFFF, (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF(( j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");

//    if ((*upto & 0xF0) == 0xE0)
//        upto++;

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;
        if ((type << 16) + field == field_id)
        {
            DBG_PRINTF("sto_subfield returned for field %u type %u\n", field_id & 0xFFFF, (field_id >> 16));
            for (int j = -5; j < 5; ++j)
                DBG_PRINTF(( j == 0 ? " [%02X] " : "  %02X  "), *(upto + j));
            DBG_PRINTF("\n");
            if (type == 0xF)    // we return arrays fully formed
                return (((int64_t)(upto - start)) << 32) /* start of the object */
                    + (uint32_t)(length);
            // return pointers to all other objects as payloads
            return (((int64_t)(upto - start + payload_start)) << 32) /* start of the object */
                + (uint32_t)(payload_length);
        }
        upto += length;
    }

    return DOESNT_EXIST;
}

// Same as subfield but indexes into a serialized array
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_subarray,
    uint32_t read_ptr, uint32_t read_len, uint32_t index_id )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 1)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + read_len;

    if ((*upto & 0xF0) == 0xF0)
        upto++;

    /*
    DBG_PRINTF("sto_subarray called, looking for index %u\n", index_id);
    for (int j = -5; j < 5; ++j)
        printf(( j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");
    */
    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;
        if (i == index_id)
        {
            DBG_PRINTF("sto_subarray returned for index %u\n", index_id);
            for (int j = -5; j < 5; ++j)
                DBG_PRINTF(( j == 0 ? " [%02X] " : "  %02X  "), *(upto + j + length));
            DBG_PRINTF("\n");

            return (((int64_t)(upto - start)) << 32) /* start of the object */
                +   (uint32_t)(length);
        }
        upto += length;
    }

    return DOESNT_EXIST;
}

// Convert an account ID into a base58-check encoded r-address
DEFINE_HOOK_FUNCTION(
    int64_t,
    util_raddr,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t read_ptr, uint32_t read_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len != 20)
        return INVALID_ARGUMENT;

    std::string raddr = base58EncodeToken(TokenType::AccountID, memory + read_ptr, read_len);

    if (write_len < raddr.size())
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        raddr.c_str(), raddr.size(),
        memory, memory_length);
}

// Convert a base58-check encoded r-address into a 20 byte account id
DEFINE_HOOK_FUNCTION(
    int64_t,
    util_accid,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t read_ptr, uint32_t read_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < 20)
        return TOO_SMALL;

    if (read_len > 49)
        return TOO_BIG;

    // RH TODO we shouldn't need to slice this input but the base58 routine fails if we dont... maybe
    // some encoding or padding that shouldnt be there or maybe something that should be there
    char buffer[50];
    for (int i = 0; i < read_len; ++i)
        buffer[i] = *(memory + read_ptr + i);
    buffer[read_len] = 0;

    std::string raddr{buffer};
    auto const result = decodeBase58Token(raddr, TokenType::AccountID);
    if (result.empty())
        return INVALID_ARGUMENT;


    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len,
        result.data(), 20,
        memory, memory_length);
}


/**
 * Inject a field into an sto if there is sufficient space
 * Field must be fully formed and wrapped (NOT JUST PAYLOAD)
 * sread - source object
 * fread - field to inject
 */
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_emplace,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t sread_ptr, uint32_t sread_len,
    uint32_t fread_ptr, uint32_t fread_len, uint32_t field_id )
{
    HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(sread_ptr, sread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(fread_ptr, fread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < sread_len + fread_len)
        return TOO_SMALL;

    // RH TODO: put these constants somewhere (votable?)
    if (sread_len > 1024*16)
        return TOO_BIG;

    if (fread_len > 4096)
        return TOO_BIG;

    // we must inject the field at the canonical location....
    // so find that location
    unsigned char* start = (unsigned char*)(memory + sread_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + sread_len;
    unsigned char* inject_start = end;
    unsigned char* inject_end = end;

    DBG_PRINTF("sto_emplace called, looking for field %u type %u\n", field_id & 0xFFFF, (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF(( j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");


    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;
        if ((type << 16) + field == field_id)
        {
            inject_start = upto;
            inject_end = upto + length;
            break;
        }
        else if ((type << 16) + field > field_id)
        {
            inject_start = upto;
            inject_end = upto;
            break;
        }
        upto += length;
    }

    // upto is injection point
    int64_t bytes_written = 0;

    // part 1
    if (inject_start - start > 0)
    {
        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr, write_len,
            start, (inject_start - start),
            memory, memory_length);
    }

    // write the field
    WRITE_WASM_MEMORY(
        bytes_written,
        (write_ptr + bytes_written), (write_len - bytes_written),
        memory + fread_ptr, fread_len,
        memory, memory_length);



    // part 2
    if (end - inject_end > 0)
    {
        WRITE_WASM_MEMORY(
            bytes_written,
            (write_ptr + bytes_written), (write_len - bytes_written),
            inject_end, (end - inject_end),
            memory, memory_length);
    }
    return bytes_written;
}

/**
 * Remove a field from an sto if the field is present
 */
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_erase,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t read_ptr,  uint32_t read_len,  uint32_t field_id )
{
    HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    // RH TODO: constants
    if (read_len > 16*1024)
        return TOO_BIG;

    if (write_len < read_len)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + read_len;
    unsigned char* erase_start = 0;
    unsigned char* erase_end = 0;

    DBG_PRINTF("sto_erase called, looking for field %u type %u\n", field_id & 0xFFFF, (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF(( j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");


    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;
        if ((type << 16) + field == field_id)
        {
            erase_start = upto;
            erase_end = upto + length;
        }
        upto += length;
    }


    if (erase_start >= start && erase_end >= start && erase_start <= end && erase_end <= end)
    {
        // do erasure via selective copy
        int64_t bytes_written = 0;

        // part 1
        if (erase_start - start > 0)
        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr, write_len,
            start, (erase_start - start),
            memory, memory_length);

        // skip the field we're erasing

        // part 2
        if (end - erase_end > 0)
        WRITE_WASM_MEMORY(
            bytes_written,
            (write_ptr + bytes_written), (write_len - bytes_written),
            erase_end, (end - erase_end),
            memory, memory_length);
        return bytes_written;
    }
    return DOESNT_EXIST;

}




DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_validate,
    uint32_t read_ptr, uint32_t read_len )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    // RH TODO: see if an internal ripple function/class would do this better

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 1)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + read_len;


    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return 0;
        upto += length;
    }

    return 1;
}



// Validate either an secp256k1 signature or an ed25519 signature, using the XRPLD convention for identifying
// the key type. Pointer prefixes: d = data, s = signature, k = public key.
DEFINE_HOOK_FUNCTION(
    int64_t,
    util_verify,
    uint32_t dread_ptr, uint32_t dread_len,
    uint32_t sread_ptr, uint32_t sread_len,
    uint32_t kread_ptr, uint32_t kread_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(dread_ptr, dread_len, memory_length) ||
        NOT_IN_BOUNDS(sread_ptr, sread_len, memory_length) ||
        NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
        return OUT_OF_BOUNDS;

    ripple::Slice keyslice  {reinterpret_cast<const void*>(kread_ptr + memory), kread_len};
    ripple::Slice data {reinterpret_cast<const void*>(dread_ptr + memory), dread_len};
    ripple::Slice sig  {reinterpret_cast<const void*>(sread_ptr + memory), sread_len};
    ripple::PublicKey key { keyslice };
    return verify(key, data, sig, false) ? 1 : 0;
}

// Return the current fee base of the current ledger (multiplied by a margin)
DEFINE_HOOK_FUNCNARG(
    int64_t,
    fee_base)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    return (int64_t)((double)(view.fees().base.drops()) * hook_api::fee_base_multiplier);
}

// Return the fee base for a hypothetically emitted transaction from the current hook based on byte count
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_fee_base,
    uint32_t tx_byte_count )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint64_t base_fee = (uint64_t)fee_base(hookCtx, memoryCtx); // will always return non-negative

    int64_t burden = etxn_burden(hookCtx, memoryCtx);
    if (burden < 1)
        return FEE_TOO_LARGE;

    uint64_t fee = base_fee * burden;
    if (fee < burden || fee & (3ULL << 62)) // a second under flow to handle
        return FEE_TOO_LARGE;

    hookCtx.fee_base = fee;

    return fee * hook_api::drops_per_byte * tx_byte_count;
}

// Populate an sfEmitDetails field in a soon-to-be emitted transaction
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_details,
    uint32_t write_ptr, uint32_t write_len )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < hook_api::etxn_details_size)
        return TOO_SMALL;

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint32_t generation = (uint32_t)(etxn_generation(hookCtx, memoryCtx)); // always non-negative so cast is safe

    int64_t burden = etxn_burden(hookCtx, memoryCtx);
    if (burden < 1)
        return FEE_TOO_LARGE;

    unsigned char* out = memory + write_ptr;

    *out++ = 0xECU; // begin sfEmitDetails                            /* upto =   0 | size =  1 */
    *out++ = 0x20U; // sfEmitGeneration preamble                      /* upto =   1 | size =  6 */
    *out++ = 0x2BU; // preamble cont
    *out++ = ( generation >> 24 ) & 0xFFU;
    *out++ = ( generation >> 16 ) & 0xFFU;
    *out++ = ( generation >>  8 ) & 0xFFU;
    *out++ = ( generation >>  0 ) & 0xFFU;
    *out++ = 0x3C; // sfEmitBurden preamble                           /* upto =   7 | size =  9 */
    *out++ = ( burden >> 56 ) & 0xFFU;
    *out++ = ( burden >> 48 ) & 0xFFU;
    *out++ = ( burden >> 40 ) & 0xFFU;
    *out++ = ( burden >> 32 ) & 0xFFU;
    *out++ = ( burden >> 24 ) & 0xFFU;
    *out++ = ( burden >> 16 ) & 0xFFU;
    *out++ = ( burden >>  8 ) & 0xFFU;
    *out++ = ( burden >>  0 ) & 0xFFU;
    *out++ = 0x5A; // sfEmitParentTxnID preamble                      /* upto =  16 | size = 33 */
    if (otxn_id(hookCtx, memoryCtx, out - memory, 32) != 32)
        return INTERNAL_ERROR;
    out += 32;
    *out++ = 0x5B; // sfEmitNonce                                     /* upto =  49 | size = 33 */
    if (nonce(hookCtx, memoryCtx, out - memory, 32) != 32)
        return INTERNAL_ERROR;
    out += 32;
    *out++ = 0x89; // sfEmitCallback preamble                         /* upto =  82 | size = 22 */
    *out++ = 0x14; // preamble cont
    if (hook_account(hookCtx, memoryCtx, out - memory, 20) != 20)
        return INTERNAL_ERROR;
    out += 20;
    *out++ = 0xE1U; // end object (sfEmitDetails)                     /* upto = 104 | size =  1 */
                                                                      /* upto = 105 | --------- */
    DBG_PRINTF("emitdetails size = %d\n", (out - memory - write_ptr));
    return 105;
}


// RH TODO: bill based on guard counts
// Guard function... very important. Enforced on SetHook transaction, keeps track of how many times a
// runtime loop iterates and terminates the hook if the iteration count rises above a preset number of iterations
// as determined by the hook developer
DEFINE_HOOK_FUNCTION(
    int32_t,
    _g,
    uint32_t id, uint32_t maxitr )
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    if (hookCtx.guard_map.find(id) == hookCtx.guard_map.end())
        hookCtx.guard_map[id] = 1;
    else
        hookCtx.guard_map[id]++;

    if (hookCtx.guard_map[id] > maxitr)
    {
        if (id > 0xFFFFU)
        {
            JLOG(j.trace())
                << "HookInfo[" << HC_ACC() << "]: Macro guard violation. "
                << "Src line: " << (id & 0xFFFFU) << " "
                << "Macro line: " << (id >> 16) << " "
                << "Iterations: " << hookCtx.guard_map[id];
        }
        else
        {
            JLOG(j.trace())
                << "HookInfo[" << HC_ACC() << "]: Guard violation. "
                << "Src line: " << id
                << "Iterations: " << hookCtx.guard_map[id];
        }
        hookCtx.result.exitType = hook_api::ExitType::ROLLBACK;
        hookCtx.result.exitCode = GUARD_VIOLATION;
        return RC_ROLLBACK;
    }
    return 1;
}

#define RETURN_IF_INVALID_FLOAT(float1)\
{\
    if (float1 < 0) return hook_api::INVALID_FLOAT;\
    if (float1 != 0)\
    {\
        int64_t mantissa = get_mantissa(float1);\
        int32_t exponent = get_exponent(float1);\
        if (mantissa < minMantissa ||\
            mantissa > maxMantissa ||\
            exponent > maxExponent ||\
            exponent < minExponent)\
            return INVALID_FLOAT;\
    }\
}


DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_float,
    uint32_t read_ptr, uint32_t read_len,
    int64_t float1)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx on current stack
    if (!j.trace())
        return 0;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (float1 == 0)
        RETURN_HOOK_TRACE(read_ptr, read_len, "Float 0*10^(0) <ZERO>");
        
    int64_t man = get_mantissa(float1);
    int32_t exp = get_exponent(float1);
    bool neg = is_negative(float1);
    if (man < minMantissa || man > maxMantissa || exp < minExponent || exp > maxExponent)
        RETURN_HOOK_TRACE(read_ptr, read_len, "Float <INVALID>");

    man *= (neg ? -1 : 1);

    RETURN_HOOK_TRACE(read_ptr, read_len, "Float " << man << "*10^(" << exp << ")");
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_set,
    int32_t exp, int64_t mantissa )
{
    if (mantissa == 0)
        return 0;

    // normalize
    while (mantissa < minMantissa)
    {
        mantissa *= 10;
        exp--;
        if (exp < minExponent)
            return INVALID_FLOAT; //underflow
    }
    while (mantissa > maxMantissa)
    {
        mantissa /= 10;
        exp++;
        if (exp > maxExponent)
            return INVALID_FLOAT; //overflow
    }

    return make_float(mantissa, exp);
}

// https://stackoverflow.com/questions/31652875/fastest-way-to-multiply-two-64-bit-ints-to-128-bit-then-to-64-bit
inline void umul64wide (uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a_lo = (uint64_t)(uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint64_t)(uint32_t)b;
    uint64_t b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint32_t cy = (uint32_t)(((p0 >> 32) + (uint32_t)p1 + (uint32_t)p2) >> 32);

    *lo = p0 + (p1 << 32) + (p2 << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + cy;
}

inline int64_t mulratio_internal
    (int64_t& man1, int32_t& exp1, bool round_up, uint32_t numerator, uint32_t denominator)
{
    try
    {
        ripple::IOUAmount amt {man1, exp1};
        ripple::IOUAmount out = ripple::mulRatio(amt, numerator, denominator, round_up != 0); // already normalized
        man1 = out.mantissa();
        exp1 = out.exponent();
        return 1;
    }
    catch (std::overflow_error& e)
    {
        return OVERFLOW;
    }
}

inline int64_t float_multiply_internal_parts(
        uint64_t man1,
        int32_t exp1,
        bool neg1,
        uint64_t man2,
        int32_t exp2,
        bool neg2)
{
    int32_t exp_out = exp1 + exp2;

    // multiply the mantissas, this could result in upto a 128 bit number, represented as high and low here
    uint64_t man_hi = 0, man_lo = 0;
    umul64wide(man1, man2, &man_hi, &man_lo);

    // normalize our double wide mantissa by shifting bits under man_hi is 0
    uint8_t man_shifted = 0;
    while (man_hi > 0)
    {
        bool set = (man_hi & 1) != 0;
        man_hi >>= 1;
        man_lo >>= 1;
        man_lo += (set ? (1ULL<<63U) : 0);
        man_shifted++;
    }

    // we shifted the mantissa by man_shifted bits, which equates to a division by 2^man_shifted
    // now shift into the normalized range
    while (man_lo > maxMantissa)
    {
        if (exp_out > maxExponent)
            return OVERFLOW;
        man_lo /= 10;
        exp_out++;
    }

    // we can adjust for the bitshifting by doing upto two smaller multiplications now
    neg1 = (neg1 && !neg2) || (!neg1 && neg2);
    int64_t man_out = (neg1 ? -1 : 1) * ((int64_t)(man_lo));
    if (man_shifted > 32)
    {
        man_shifted -=32;
        if (mulratio_internal(man_out, exp_out, false, 0xFFFFFFFFU, 1) < 0)
            return OVERFLOW;
    }

    if (mulratio_internal(man_out, exp_out, false, 1U << man_shifted, 1) < 0)
        return OVERFLOW;

    // now we have our product
    return make_float(man_out, exp_out);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_int,
    int64_t float1,
    uint32_t decimal_places,
    uint32_t absolute)
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0) return 0;
    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    bool neg1 = is_negative(float1);

    if (neg1 && !absolute)
        return CANT_RETURN_NEGATIVE;

    while (exp1 > -6)
    {
        man1 *= 10;
        exp1--;
    }

    while (exp1 < -6)
    {
        man1 /= 10;
        exp1++;
    }
    if (((int64_t)(man1)) < man1)
        return INVALID_FLOAT;

    return man1;

}


DEFINE_HOOK_FUNCTION(
    int64_t,
    float_multiply,
    int64_t float1, int64_t float2 )
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    if (float1 == 0 || float2 == 0) return 0;

    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    bool neg1 = is_negative(float1);
    uint64_t man2 = get_mantissa(float2);
    int32_t exp2 = get_exponent(float2);
    bool neg2 = is_negative(float2);


    return float_multiply_internal_parts(man1, exp1, neg1, man2, exp2, neg2);
}


DEFINE_HOOK_FUNCTION(
    int64_t,
    float_mulratio,
    int64_t float1, uint32_t round_up,
    uint32_t numerator, uint32_t denominator )
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    if (denominator == 0)
        return DIVISION_BY_ZERO;

    int64_t man1 = (int64_t)(get_mantissa(float1)) * (is_negative(float1) ? -1 : 1);
    int32_t exp1 = get_exponent(float1);

    if (mulratio_internal(man1, exp1, round_up > 0, numerator, denominator) < 0)
        return OVERFLOW;

    return make_float(man1, exp1);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_negate,
    int64_t float1 )
{
    if (float1 == 0) return 0;
    RETURN_IF_INVALID_FLOAT(float1);
    return hook_float::invert_sign(float1);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_compare,
    int64_t float1, int64_t float2, uint32_t mode)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    bool equal_flag     = mode & compare_mode::EQUAL;
    bool less_flag      = mode & compare_mode::LESS;
    bool greater_flag   = mode & compare_mode::GREATER;
    bool not_equal      = less_flag && greater_flag;

    if ((equal_flag && less_flag && greater_flag) || mode == 0)
        return INVALID_ARGUMENT;

    try
    {
        int64_t man1 = (int64_t)(get_mantissa(float1)) * (is_negative(float1) ? -1 : 1);
        int32_t exp1 = get_exponent(float1);
        ripple::IOUAmount amt1 {man1, exp1};
        int64_t man2 = (int64_t)(get_mantissa(float2)) * (is_negative(float2) ? -1 : 1);
        int32_t exp2 = get_exponent(float2);
        ripple::IOUAmount amt2 {man2, exp2};

        if (not_equal && amt1 != amt2)
            return 1;

        if (equal_flag && amt1 == amt2)
            return 1;

        if (greater_flag && amt1 > amt2)
            return 1;

        if (less_flag && amt1 < amt2)
            return 1;

        return 0;
    }
    catch (std::overflow_error& e)
    {
        return OVERFLOW;
    }

}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sum,
    int64_t float1, int64_t float2)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    if (float1 == 0) return float2;
    if (float2 == 0) return float1;

    int64_t man1 = (int64_t)(get_mantissa(float1)) * (is_negative(float1) ? -1 : 1);
    int32_t exp1 = get_exponent(float1);
    int64_t man2 = (int64_t)(get_mantissa(float2)) * (is_negative(float2) ? -1 : 1);
    int32_t exp2 = get_exponent(float2);

    try
    {
        ripple::IOUAmount amt1 {man1, exp1};
        ripple::IOUAmount amt2 {man2, exp2};
        amt1 += amt2;
        return make_float(amt1);
    }
    catch (std::overflow_error& e)
    {
        return OVERFLOW;
    }
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sto,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t cread_ptr, uint32_t cread_len,
    uint32_t iread_ptr, uint32_t iread_len,
    int64_t float1,     uint32_t field_code)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack
    RETURN_IF_INVALID_FLOAT(float1);

    uint16_t field = field_code & 0xFFFFU;
    uint16_t type  = field_code >> 16U;

    bool is_xrp = field_code == 0;
    bool is_short = field_code == 0xFFFFFFFFU;   // non-xrp value but do not output header or tail, just amount

    int bytes_needed = 8 +
                            ( field == 0  && type == 0  ? 0 :
                            ( field == 0xFFFFU  && type == 0xFFFFU  ? 0 :
                            ( field <  16 && type <  16 ? 1 :
                            ( field >= 16 && type <  16 ? 2 :
                            ( field <  16 && type >= 16 ? 2 : 3 )))));

    int64_t bytes_written = 0;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (!is_xrp && !is_short && (cread_ptr == 0 && cread_len == 0 && iread_ptr == 0 && iread_len == 0))
        return INVALID_ARGUMENT;

    if (!is_xrp && !is_short)
    {
        if (NOT_IN_BOUNDS(cread_ptr, cread_len, memory_length) ||
            NOT_IN_BOUNDS(iread_ptr, iread_len, memory_length))
            return OUT_OF_BOUNDS;

        if (cread_len != 20 || iread_len != 20)
            return INVALID_ARGUMENT;

        bytes_needed += 40;

    }

    if (bytes_needed > write_len)
        return TOO_SMALL;

    if (is_xrp || is_short)
    {
        // do nothing
    }
    else if (field < 16 && type < 16)
    {
        *(memory + write_ptr) = (((uint8_t)type) << 4U) + ((uint8_t)field);
        bytes_written++;
    }
    else if (field >= 16 && type < 16)
    {
        *(memory + write_ptr) = (((uint8_t)type) << 4U);
        *(memory + write_ptr + 1) = ((uint8_t)field);
        bytes_written += 2;
    }
    else if (field < 16 && type >= 16)
    {
        *(memory + write_ptr) = (((uint8_t)field) << 4U);
        *(memory + write_ptr + 1) = ((uint8_t)type);
        bytes_written += 2;
    }
    else
    {
        *(memory + write_ptr) = 0;
        *(memory + write_ptr + 1) = ((uint8_t)type);
        *(memory + write_ptr + 2) = ((uint8_t)field);
        bytes_written += 3;
    }

    uint64_t man = get_mantissa(float1);
    int32_t exp = get_exponent(float1);
    bool neg = is_negative(float1);
    uint8_t out[8];
    if (is_xrp)
    {
        // we need to normalize to exp -6
        while (exp < -6)
        {
            man /= 10;
            exp++;
        }

        while (exp > -6)
        {
            man *= 10;
            exp--;
        }

        out[0] = (neg ? 0b00000000U : 0b01000000U);
        out[0] += (uint8_t)((man >> 56U) & 0b111111U);
        out[1]  = (uint8_t)((man >> 48U) & 0xFF);
        out[2]  = (uint8_t)((man >> 40U) & 0xFF);
        out[3]  = (uint8_t)((man >> 32U) & 0xFF);
        out[4]  = (uint8_t)((man >> 24U) & 0xFF);
        out[5]  = (uint8_t)((man >> 16U) & 0xFF);
        out[6]  = (uint8_t)((man >>  8U) & 0xFF);
        out[7]  = (uint8_t)((man >>  0U) & 0xFF);

    }
    else if (man == 0)
    {
        out[0] = 0b11000000U;
        for (int i = 1; i < 8; ++i)
            out[i] = 0;
    }
    else
    {

        exp += 97;

        /// encode the rippled floating point sto format

        out[0] = (neg ? 0b10000000U : 0b11000000U);
        out[0] += (uint8_t)(exp >> 2U);
        out[1] =  ((uint8_t)(exp & 0b11U)) << 6U;
        out[1] += (((uint8_t)(man >> 48U)) & 0b111111U);
        out[2] = (uint8_t)((man >> 40U) & 0xFFU);
        out[3] = (uint8_t)((man >> 32U) & 0xFFU);
        out[4] = (uint8_t)((man >> 24U) & 0xFFU);
        out[5] = (uint8_t)((man >> 16U) & 0xFFU);
        out[6] = (uint8_t)((man >>  8U) & 0xFFU);
        out[7] = (uint8_t)((man >>  0U) & 0xFFU);
    }

    WRITE_WASM_MEMORY(
        bytes_written,
        write_ptr + bytes_written, write_len - bytes_written,
        out, 8,
        memory, memory_length);

    if (!is_xrp && !is_short)
    {
        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr + bytes_written, write_len - bytes_written,
            memory + cread_ptr, 20,
            memory, memory_length);

        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr + bytes_written, write_len - bytes_written,
            memory + iread_ptr, 20,
            memory, memory_length);
    }

    return bytes_written;

}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sto_set,
    uint32_t read_ptr, uint32_t read_len )
{

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (read_len < 8)
        return NOT_AN_OBJECT;

    uint8_t* upto = memory + read_ptr;

    if (read_len > 8)
    {
        uint8_t hi = memory[read_ptr] >> 4U;
        uint8_t lo = memory[read_ptr] & 0xFU;

        if (hi == 0 && lo == 0)
        {
            // typecode >= 16 && fieldcode >= 16
            if (read_len < 11)
                return NOT_AN_OBJECT;
            upto += 3;
        }
        else if (hi == 0 || lo == 0)
        {
            // typecode >= 16 && fieldcode < 16
            if (read_len < 10)
                return NOT_AN_OBJECT;
            upto += 2;
        }
        else
        {
            // typecode < 16 && fieldcode < 16
            upto++;
        }
    }


    bool is_negative = (((*upto) & 0b01000000U) == 0);
    int32_t exponent = (((*upto++) & 0b00111111U)) << 2U;
    exponent += ((*upto)>>6U);
    exponent -= 97;
    uint64_t mantissa = (((uint64_t)(*upto++)) & 0b00111111U) << 48U;
    mantissa += ((uint64_t)*upto++) << 40U;
    mantissa += ((uint64_t)*upto++) << 32U;
    mantissa += ((uint64_t)*upto++) << 24U;
    mantissa += ((uint64_t)*upto++) << 16U;
    mantissa += ((uint64_t)*upto++) <<  8U;
    mantissa += ((uint64_t)*upto++);

    if (mantissa == 0)
        return 0;

    return hook_float::float_set(exponent, (is_negative ? -1 : 1) * ((int64_t)(mantissa)));
}
inline int64_t float_divide_internal(int64_t float1, int64_t float2)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);
    if (float2 == 0)
        return DIVISION_BY_ZERO;
    if (float1 == 0)
        return 0;

    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    bool neg1 = is_negative(float1);
    uint64_t man2 = get_mantissa(float2);
    int32_t exp2 = get_exponent(float2);
    bool neg2 = is_negative(float2);

    while (man1 > maxMantissa)
    {
        man1 /= 10;
        exp1++;
        if (exp1 > maxExponent)
            return INVALID_FLOAT;
    }

    while (man1 < minMantissa)
    {
        man1 *= 10;
        exp1--;
        if (exp1 < minExponent)
            return 0;
    }


    while (man2 > man1)
    {
        man2 /= 10;
        exp2++;
    }

    if (man2 == 0)
        return DIVISION_BY_ZERO;

    while (man2 < man1)
    {
        if (man2*10 > man1)
            break;
        man2 *= 10;
        exp2--;
    }

    uint64_t man3 = 0;
    int32_t exp3 = exp1 - exp2;
    while (man2 > 0)
    {
        int i = 0;
        for (; man1 > man2; man1 -= man2, ++i);

        man3 *= 10;
        man3 += i;
        man2 /= 10;
        if (man2 == 0)
            break;
        exp3--;
    }

    // normalize
    while (man3 < minMantissa)
    {
        man3 *= 10;
        exp3--;
        if (exp3 < minExponent)
            return 0;
    }

    while (man3 > maxMantissa)
    {
        man3 /= 10;
        exp3++;
        if (exp3 > maxExponent)
            return INVALID_FLOAT;
    }

    bool neg3 = !((neg1 && neg2) || (!neg1 && !neg2));
    int64_t float_out = set_sign(0, neg3);
    float_out = set_exponent(float_out, exp3);
    float_out = set_mantissa(float_out, man3);
    return float_out;

}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_divide,
    int64_t float1, int64_t float2 )
{
    return float_divide_internal(float1, float2);
}
const int64_t float_one_internal = make_float(1000000000000000ull, -15);


DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sign_set,
    int64_t float1,     uint32_t negative)
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return set_sign(float1, negative != 0);
}
DEFINE_HOOK_FUNCNARG(
    int64_t,
    float_one)
{
    return float_one_internal;
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_invert,
    int64_t float1 )
{
    if (float1 == 0)
        return DIVISION_BY_ZERO;
    return float_divide_internal(float_one_internal, float1);
}
DEFINE_HOOK_FUNCTION(
    int64_t,
    float_exponent,
    int64_t float1 )
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return get_exponent(float1);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_mantissa,
    int64_t float1 )
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return get_mantissa(float1);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sign,
    int64_t float1 )
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return is_negative(float1);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_exponent_set,
    int64_t float1, int32_t exponent )
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return set_exponent(float1, exponent);
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_mantissa_set,
    int64_t float1, int64_t mantissa )
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (mantissa == 0)
        return 0;
    return set_mantissa(float1, mantissa);
}


