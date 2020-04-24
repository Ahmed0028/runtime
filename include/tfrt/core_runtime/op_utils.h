/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- op_utils.h - Helpers for op implementations --------------*- C++ -*-===//
//
// This file declares simple helper routines to make it easier to write metadata
// function and dispatch function for a op. This is intended to be small and
// simple things and is nearly header-only.
//
//===----------------------------------------------------------------------===//

// TODO(fishx): Split this file into metadata_utils.h and dispatch_utils.h.

#ifndef TFRT_CORE_RUNTIME_OP_UTILS_H_
#define TFRT_CORE_RUNTIME_OP_UTILS_H_

#include <tuple>

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/Support/Error.h"
#include "tfrt/core_runtime/op_args.h"
#include "tfrt/core_runtime/op_attrs.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/kernel_frame.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/host_context/location.h"
#include "tfrt/support/rc_array.h"
#include "tfrt/tensor/tensor_metadata.h"

namespace tfrt {
class Tensor;
class TensorMetadata;

//===----------------------------------------------------------------------===//
// Registration helpers used to make metadata function easier to define.
//===----------------------------------------------------------------------===//

// TFRT_METADATA is a macro that makes defining metadata function more
// straightforward. Example:
//
//   RCReference<AsyncValue> AddMetadataFn(const TensorMetadata& a,
//                                         const TensorMetadata& b,
//                                         TensorMetadata* c,
//                                         Location loc) {
//     // Check argument metadata.
//     if (error) return loc.EmitErrorAsync("error");
//     *result = TensorMetadata(a.dtype, a.shape);
//     return {};
//   }
//
// Example for metadata function that needs OpAttrs:
//
//   void ExampleMetadataFn(const TensorMetadata argument,
//                          const OpAttrsRef& attrs,
//                          TensorMetadata* result,
//                          Location loc) { ... }
//
#define TFRT_METADATA(...) \
  ::tfrt::MetadataFnImpl<decltype(&__VA_ARGS__), &__VA_ARGS__>::Invoke

//===----------------------------------------------------------------------===//
// Registration helpers used to make cpu dispatch function easier to define.
//===----------------------------------------------------------------------===//

// TFRT_CPU_OP is a macro that makes defining dispatch
// function more straightforward. Example:
//
//   void AddDispatchFn(const HostTensor& a,
//                      const HostTensor& b,
//                      const TensorMetadata& c_md,
//                      HostTensor* c,
//                      Location loc) { ... }
//
// Example for dispatch function that needs OpAttrs:
//
//   void ExampleDispatchFn(const HostTensor& a,
//                          const HostTensor& b,
//                          const OpAttrsRef& attrs,
//                          const TensorMetadata& c_md,
//                          HostTensor* c,
//                          Location loc) { ... }
//
// TODO(fishx): Move it to backends/cpu.
#define TFRT_CPU_OP(...)                                      \
  ::tfrt::DispatchFnImpl<HostContext, decltype(&__VA_ARGS__), \
                         &__VA_ARGS__>::Invoke

// This class is an implementation detail of TFRT_METADATA.
template <typename F, F f>
struct MetadataFnImpl;

template <typename ReturnT, typename... Args, ReturnT (*impl_fn)(Args...)>
struct MetadataFnImpl<ReturnT (*)(Args...), impl_fn> {
  static RCReference<AsyncValue> Invoke(ArrayRef<TensorMetadata> arguments,
                                        const OpAttrsRef& attrs,
                                        MutableArrayRef<TensorMetadata> results,
                                        Location loc) {
    return HandleReturn(
        MetadataFnCallHelper<Args...>::template Invoke<0, 0, false>(
            arguments, attrs, results, loc),
        results, loc);
  }

 private:
  static RCReference<AsyncValue> HandleReturn(
      RCReference<AsyncValue> v, MutableArrayRef<TensorMetadata> results,
      Location loc) {
    return v;
  }

  template <typename T>
  static RCReference<AsyncValue> HandleReturn(
      llvm::Expected<T>&& v, MutableArrayRef<TensorMetadata> results,
      Location loc) {
    if (v) {
      return HandleReturn(std::move(*v), results, loc);
    } else {
      return loc.EmitErrorAsync(v.takeError());
    }
  }

  static RCReference<AsyncValue> HandleReturn(
      TensorMetadata&& v, MutableArrayRef<TensorMetadata> results,
      Location loc) {
    assert(results.size() == 1 && "Incorrect number of return values");
    results[0] = std::move(v);
    return {};
  }

  template <typename... T>
  static RCReference<AsyncValue> HandleReturn(
      std::tuple<T...>&& t, MutableArrayRef<TensorMetadata> results,
      Location loc) {
    assert(results.size() == sizeof...(T) &&
           "Incorrect number of return values");
    EmplaceTupleResult(results, std::move(t),
                       std::make_index_sequence<sizeof...(T)>{});
    return {};
  }

  // Helper function for storing multiple return values in std::tuple<> in the
  // results ArrayRef.
  template <typename TupleT, size_t... I>
  static void EmplaceTupleResult(MutableArrayRef<TensorMetadata> results,
                                 TupleT&& result, std::index_sequence<I...>) {
    // Use braced-init-list to retrieve the results in the tuple in sequence.
    std::ignore = std::initializer_list<int>{
        (results[I] = std::get<I>(std::forward<TupleT>(result)), 0)...};
  }

  // Helper that introspects the MetadataFn's arguments to derive the signature
  // and pass arguments, attributes, results and location to impl_fn. Works by
  // recursively unpacking the MetadataFn's arguments.
  template <typename... RemainingArgs>
  struct MetadataFnCallHelper;

  // Specialization for passing a TensorMetadata argument.
  template <typename... RemainingArgs>
  struct MetadataFnCallHelper<const TensorMetadata&, RemainingArgs...> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... pargs) {
      static_assert(arg_idx != -1,
                    "Do not place argument TensorMetadata after OptionalOpArg "
                    "or VariadicOpArg");
      static_assert(!has_attrs,
                    "Do not place argument TensorMetadata after OpAttrsRef");
      static_assert(
          result_idx == 0,
          "Do not place argument TensorMetadata after result TensorMetadata");
      assert(arg_idx < arguments.size());
      const TensorMetadata& argument = arguments[arg_idx];
      return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
          arg_idx + 1, result_idx, has_attrs>(arguments, attrs, results, loc,
                                              pargs..., argument);
    }
  };

  // Specialization for passing an optional argument.
  template <typename Head, typename... RemainingArgs>
  struct MetadataFnCallHelper<OptionalOpArg<Head>, RemainingArgs...> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... pargs) {
      static_assert(arg_idx != -1,
                    "Do not use more than one OptionalOpArg, or more than one "
                    "VariadicOpArg, or mix OptionalOpArg and VariadicOpArg");
      static_assert(
          !has_attrs,
          "Do not place optional argument TensorMetadata after OpAttrsRef");
      static_assert(result_idx == 0,
                    "Do not place optional argument TensorMetadata after "
                    "result TensorMetadata");
      assert(arg_idx == arguments.size() - 1 || arg_idx == arguments.size());
      if (arg_idx < arguments.size()) {
        const TensorMetadata& argument = arguments[arg_idx];
        return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
            -1, result_idx, has_attrs>(arguments, attrs, results, loc, pargs...,
                                       &argument);
      } else {
        return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
            -1, result_idx, has_attrs>(arguments, attrs, results, loc, pargs...,
                                       OptionalOpArg<Head>());
      }
    }
  };

  // Specialization for passing a variadic argument.
  template <typename Head, typename... RemainingArgs>
  struct MetadataFnCallHelper<VariadicOpArg<Head>, RemainingArgs...> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... pargs) {
      static_assert(arg_idx != -1,
                    "Do not use more than one OptionalOpArg, or more than one "
                    "VariadicOpArg, or mix OptionalOpArg and VariadicOpArg");
      static_assert(
          !has_attrs,
          "Do not place optional argument TensorMetadata after OpAttrsRef");
      static_assert(result_idx == 0,
                    "Do not place optional argument TensorMetadata after "
                    "result TensorMetadata");
      assert(arg_idx <= arguments.size());
      auto var_args = arguments.drop_front(arg_idx);
      return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
          -1, result_idx, has_attrs>(arguments, attrs, results, loc, pargs...,
                                     VariadicOpArg<TensorMetadata>(var_args));
    }
  };

  // Specialization for passing an OpAttrsRef.
  template <typename... RemainingArgs>
  struct MetadataFnCallHelper<const OpAttrsRef&, RemainingArgs...> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... pargs) {
      static_assert(!has_attrs, "Do not place more than one OpAttrsRef");
      static_assert(
          result_idx == 0,
          "Do not place argument OpAttrsRef after result TensorMetadata");
      return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
          arg_idx, result_idx, true>(arguments, attrs, results, loc, pargs...,
                                     attrs);
    }
  };

  // Specialization for passing a TensorMetadata result.
  template <typename... RemainingArgs>
  struct MetadataFnCallHelper<TensorMetadata*, RemainingArgs...> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... pargs) {
      assert(result_idx < results.size());
      TensorMetadata* result = &results[result_idx];
      return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
          arg_idx, result_idx + 1, has_attrs>(arguments, attrs, results, loc,
                                              pargs..., result);
    }
  };

  // Specialization for passing a Location.
  template <typename... RemainingArgs>
  struct MetadataFnCallHelper<Location, RemainingArgs...> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... pargs) {
      return MetadataFnCallHelper<RemainingArgs...>::template Invoke<
          arg_idx, result_idx, has_attrs>(arguments, attrs, results, loc,
                                          pargs..., loc);
    }
  };

  // Base case: No arguments left.
  template <>
  struct MetadataFnCallHelper<> {
    template <int arg_idx, int result_idx, bool has_attrs,
              typename... PreviousArgs>
    static ReturnT Invoke(ArrayRef<TensorMetadata> arguments,
                          const OpAttrsRef& attrs,
                          MutableArrayRef<TensorMetadata> results, Location loc,
                          const PreviousArgs&... args) {
      assert((arg_idx == arguments.size() || arg_idx == -1) &&
             "Extra arguments passed to metadata function.");
      assert((result_idx == results.size() || result_idx == 0) &&
             "Extra results passed to metadata function.");
      return impl_fn(args...);
    }
  };
};

// This class is an implementation detail of TFRT_CPU_OP.
template <typename DeviceContext, typename F, F f>
struct DispatchFnImpl;

template <typename DeviceContext, typename Return, typename... Args,
          Return (*impl_fn)(Args...)>
struct DispatchFnImpl<DeviceContext, Return (*)(Args...), impl_fn> {
  static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                     const OpAttrsRef& attrs,
                     ArrayRef<TensorMetadata> result_mds,
                     MutableArrayRef<RCReference<AsyncValue>> results,
                     AsyncValueRef<Chain>* chain, Location loc,
                     HostContext* host) {
    DispatchFnCallHelper<true, Args...>::template Invoke<0, 0, 0, false, false>(
        ctx, arguments, attrs, result_mds, results, chain, loc, host);
  }

 protected:
  // Helper that introspects the DispatchFn's arguments to derive the signature
  // and pass arguments, attributes, results, out_chain and location to impl_fn.
  // Works by recursively unpacking the DispatchFn's arguments.
  //
  // NOTE(fishx): The specification will only be used if Enable is true. We use
  // it to achieve similar functionality as std::enable_if, which we cannot use
  // due to the repeated types.
  template <bool Enable, typename... RemainingArgs>
  struct DispatchFnCallHelper;

  // The return value is AsyncValueRef<T>.
  template <int result_idx, bool has_chain, typename T>
  struct DispatchReturnHelper {
    static void Invoke(MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const Args&... args) {
      HandleReturn<result_idx, has_chain>(results, chain, loc, host,
                                          impl_fn(args...));
    }
  };

  // The return value is void.
  template <int result_idx, bool has_chain>
  struct DispatchReturnHelper<result_idx, has_chain, void> {
    static void Invoke(MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const Args&... args) {
      assert(result_idx == results.size() &&
             "Extra results passed to dispatch function.");
      impl_fn(args...);
    }
  };

  // For ops functions that return T.
  template <int result_idx, bool has_chain, typename T>
  static void HandleReturn(MutableArrayRef<RCReference<AsyncValue>> results,
                           AsyncValueRef<Chain>* chain, Location loc,
                           HostContext* host, T&& t) {
    static_assert(result_idx == 0,
                  "Do not both have result argument and return result");
    assert(results.size() == 1 && "Incorrect number of return value");
    results[0] =
        loc.GetHost()->MakeConcreteAsyncValueRef<T>(std::forward<T>(t));
  }

  // For ops functions that return AsyncValueRef<Chain>.
  template <int result_idx, bool has_chain>
  static void HandleReturn(MutableArrayRef<RCReference<AsyncValue>> results,
                           AsyncValueRef<Chain>* chain, Location loc,
                           HostContext* host, AsyncValueRef<Chain>&& t) {
    static_assert(!has_chain,
                  "Do not both have chain argument and return chain");
    assert(result_idx == results.size() &&
           "Extra results passed to dispatch function.");
    *chain = std::move(t);
  }

  // For ops functions that return AsyncValueRef<T>.
  template <int result_idx, bool has_chain, typename T>
  static void HandleReturn(MutableArrayRef<RCReference<AsyncValue>> results,
                           AsyncValueRef<Chain>* chain, Location loc,
                           HostContext* host, AsyncValueRef<T> t) {
    static_assert(result_idx == 0,
                  "Do not both have result argument and return result");
    assert(results.size() == 1 && "Incorrect number of return value");
    results[0] = std::move(t);
  }

  // For ops functions that return RCReference<AsyncValue>.
  template <int result_idx, bool has_chain>
  static void HandleReturn(MutableArrayRef<RCReference<AsyncValue>> results,
                           AsyncValueRef<Chain>* chain, Location loc,
                           HostContext* host, RCReference<AsyncValue> t) {
    static_assert(result_idx == 0,
                  "Do not both have result argument and return result");
    assert(results.size() == 1 && "Incorrect number of return value");
    // Add location information to error result if necessary.
    t->AndThen([t = t.get(), loc] {
      if (t->IsError()) {
        t->SetErrorLocationIfUnset(loc.Decode());
        loc.GetHost()->EmitError(t->GetError());
      }
    });
    results[0] = std::move(t);
  }

  // For ops functions that return Expected<T>.
  template <int result_idx, bool has_chain, typename T>
  static void HandleReturn(MutableArrayRef<RCReference<AsyncValue>> results,
                           AsyncValueRef<Chain>* chain, Location loc,
                           HostContext* host, Expected<T>&& t) {
    static_assert(result_idx == 0,
                  "Do not both have result argument and return result");
    if (t) {
      HandleReturn<result_idx, has_chain>(results, chain, loc, host,
                                          std::move(*t));
    } else {
      results[0] = loc.EmitErrorAsync(t.takeError());
      for (size_t i = 1, e = results.size(); i != e; ++i) {
        results[i] = results[0].CopyRef();
      }
    }
  }

  // For kernel functions that return std::tuple<>, store the results in order
  // as the output AsyncValues in the KernelFrame.
  template <int result_idx, bool has_chain, typename... T>
  static void HandleReturn(MutableArrayRef<RCReference<AsyncValue>> results,
                           AsyncValueRef<Chain>* chain, Location loc,
                           HostContext* host, std::tuple<T...>&& t) {
    assert(results.size() == sizeof...(T) &&
           "Incorrect number of results passed to op.");
    EmplaceTupleResult(results, host, std::move(t),
                       std::make_index_sequence<sizeof...(T)>{});
  }

  // Helper function for storing multiple return values in std::tuple<> as
  // output AsyncValue in KernelFrame.
  template <typename TupleT, size_t... I>
  static void EmplaceTupleResult(
      MutableArrayRef<RCReference<AsyncValue>> results, HostContext* host,
      TupleT&& result, std::index_sequence<I...>) {
    // Use braced-init-list to retrieve the results in the tuple in sequence.
    std::ignore = std::initializer_list<int>{
        (results[I] =
             host->MakeConcreteAsyncValueRef<std::tuple_element_t<I, TupleT>>(
                 std::get<I>(std::forward<TupleT>(result))),
         0)...};
  }

  // Specialization for passing OpAttrsRef.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<true, const OpAttrsRef&, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(!has_attrs, "Do not place more than one OpAttrsRef");
      static_assert(!has_chain, "Do not place argument OpAttrsRef after chain");
      static_assert(result_idx == 0,
                    "Do not place OpAttrsRef after result Tensor");
      static_assert(md_idx == 0,
                    "Do not place OpAttrsRef after result Metadata");
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx, md_idx, true, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., attrs);
    }
  };

  // Specialization for passing a Metadata result.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<true, const TensorMetadata&, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(result_idx == 0,
                    "Do not place result Metadata after result Tensor");
      static_assert(!has_chain, "Do not place result Metadata after chain");
      assert(md_idx < result_mds.size());
      const TensorMetadata& md = result_mds[md_idx];
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx, md_idx + 1, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., md);
    }
  };

  // Specialization for passing a Tensor result.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<true, RCReference<AsyncValue>*,
                              RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(!has_chain, "Do not place result Tensor after chain");
      assert(result_idx < results.size());
      RCReference<AsyncValue>* arg = &results[result_idx];
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx + 1, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., arg);
    }
  };

  // Specialization for passing out_chain.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<true, AsyncValueRef<Chain>*, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(!has_chain, "Do not place more than one chain");
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx, md_idx, has_attrs, true>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., chain);
    }
  };

  // Specialization for passing Location.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<true, Location, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., loc);
    }
  };

  // Specialization for passing HostContext*.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<true, HostContext*, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., host);
    }
  };

  // Specialization for passing DeviceContext*.
  template <typename... RemainingArgs>
  struct DispatchFnCallHelper<!std::is_same<DeviceContext, HostContext>::value,
                              DeviceContext*, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., ctx);
    }
  };

  // Specialization for passing a Tensor pointer argument.
  template <typename Head, typename... RemainingArgs>
  struct DispatchFnCallHelper<true, Head*, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(std::is_base_of<Tensor, Head>::value,
                    "Only support Tensor argument");
      static_assert(!has_chain, "Do not place argument Tensor after chain");
      static_assert(!has_attrs,
                    "Do not place argument Tensor after OpAttrsRef");
      static_assert(result_idx == 0,
                    "Do not place argument Tensor after result Tensor");
      static_assert(md_idx == 0,
                    "Do not place argument Tensor after result Metadata");
      assert(arg_idx < arguments.size());

      // The CPU device will implicitly convert argument for us in the future.
      Head& arg = arguments[arg_idx]->get<Head>();
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx + 1, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., &arg);
    }
  };

  // Specialization for passing a const ref Tensor argument.
  template <typename Head, typename... RemainingArgs>
  struct DispatchFnCallHelper<true, const Head&, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(std::is_base_of<Tensor, Head>::value,
                    "Only support Tensor argument");
      static_assert(!has_chain, "Do not place argument Tensor after chain");
      static_assert(!has_attrs,
                    "Do not place argument Tensor after OpAttrsRef");
      static_assert(result_idx == 0,
                    "Do not place argument Tensor after result Tensor");
      static_assert(md_idx == 0,
                    "Do not place argument Tensor after result Metadata");
      assert(arg_idx < arguments.size());

      // The CPU device will implicitly convert argument for us in the future.
      Head& arg = arguments[arg_idx]->get<Head>();
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx + 1, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., arg);
    }
  };

  // Specialization for passing an argument.
  template <typename Head, typename... RemainingArgs>
  struct DispatchFnCallHelper<true, Argument<Head>, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      static_assert(std::is_base_of<Tensor, Head>::value,
                    "Only support Tensor argument");
      static_assert(!has_chain, "Do not place argument Tensor after chain");
      static_assert(!has_attrs,
                    "Do not place argument Tensor after OpAttrsRef");
      static_assert(result_idx == 0,
                    "Do not place argument Tensor after result Tensor");
      static_assert(md_idx == 0,
                    "Do not place argument Tensor after result Metadata");
      assert(arg_idx < arguments.size());

      // The CPU device will implicitly convert argument for us in the future.
      Argument<Head> arg(arguments[arg_idx]);
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          arg_idx + 1, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., arg);
    }
  };

  // Specialization for passing an optional ref Tensor argument.
  template <typename Head, typename... RemainingArgs>
  struct DispatchFnCallHelper<true, OptionalOpArg<Head>, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      // TODO(jingdong): Deduplicate static_assert checks across other template
      // specializations.
      static_assert(std::is_base_of<Tensor, Head>::value,
                    "Only support optional Tensor argument");
      static_assert(arg_idx != -1,
                    "Do not use more than one OptionalOpArg, or more than one "
                    "VariadicOpArg, or mix OptionalOpArg and VariadicOpArg");
      static_assert(!has_chain,
                    "Do not place optional Tensor argument after chain");
      static_assert(!has_attrs,
                    "Do not place optional Tensor argument after OpAttrsRef");
      static_assert(
          result_idx == 0,
          "Do not place optional Tensor argument after result Tensor");
      static_assert(
          md_idx == 0,
          "Do not place optional Tensor argument after result Metadata");

      assert(arg_idx == arguments.size() - 1 || arg_idx == arguments.size());
      if (arg_idx < arguments.size()) {
        // The GPU device will implicitly convert argument for us in the future.
        Head* arg = &arguments[arg_idx]->get<Head>();
        // TODO(b/146386166): Emit error instead of assert().
        assert(arg && "requires Tensor input");
        DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
            -1, result_idx, md_idx, has_attrs, has_chain>(
            ctx, arguments, attrs, result_mds, results, chain, loc, host,
            pargs..., arg);
      } else {
        DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
            -1, result_idx, md_idx, has_attrs, has_chain>(
            ctx, arguments, attrs, result_mds, results, chain, loc, host,
            pargs..., OptionalOpArg<Head>());
      }
    }
  };

  // Specialization for passing a variadic ref Tensor argument.
  template <typename Head, typename... RemainingArgs>
  struct DispatchFnCallHelper<true, RepeatedArguments<Head>, RemainingArgs...> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... pargs) {
      // TODO(jingdong): Deduplicate static_assert checks across other template
      // specializations.
      static_assert(std::is_base_of<Tensor, Head>::value,
                    "Only support optional Tensor argument");
      static_assert(arg_idx != -1,
                    "Do not use more than one OptionalOpArg, or more than one "
                    "VariadicOpArg, or mix OptionalOpArg and VariadicOpArg");
      static_assert(!has_chain,
                    "Do not place optional Tensor argument after chain");
      static_assert(!has_attrs,
                    "Do not place optional Tensor argument after OpAttrsRef");
      static_assert(
          result_idx == 0,
          "Do not place optional Tensor argument after result Tensor");
      static_assert(
          md_idx == 0,
          "Do not place optional Tensor argument after result Metadata");

      assert(arg_idx <= arguments.size());
      ArrayRef<AsyncValue*> var_tensor_args = arguments.drop_front(arg_idx);
      DispatchFnCallHelper<true, RemainingArgs...>::template Invoke<
          -1, result_idx, md_idx, has_attrs, has_chain>(
          ctx, arguments, attrs, result_mds, results, chain, loc, host,
          pargs..., RepeatedArguments<Head>(var_tensor_args));
    }
  };

  // Base case: No arguments left.
  template <>
  struct DispatchFnCallHelper<true> {
    template <int arg_idx, int result_idx, int md_idx, bool has_attrs,
              bool has_chain, typename... PreviousArgs>
    static void Invoke(DeviceContext* ctx, ArrayRef<AsyncValue*> arguments,
                       const OpAttrsRef& attrs,
                       ArrayRef<TensorMetadata> result_mds,
                       MutableArrayRef<RCReference<AsyncValue>> results,
                       AsyncValueRef<Chain>* chain, Location loc,
                       HostContext* host, const PreviousArgs&... args) {
      // TODO(b/146386166): Emit error instead of assert().
      assert((arg_idx == arguments.size() || arg_idx == -1) &&
             "Extra arguments passed to dispatch function.");
      assert((md_idx == result_mds.size() || md_idx == 0) &&
             "Extra result Metadatas passed to dispatch function.");
      DispatchReturnHelper<result_idx, has_chain, Return>::Invoke(
          results, chain, loc, host, args...);
    }
  };
};
}  //  namespace tfrt
#endif  // TFRT_CORE_RUNTIME_OP_UTILS_H_