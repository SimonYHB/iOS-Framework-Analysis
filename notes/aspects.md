> 本篇是笔者解读源码项目 [iOS-Framework-Analysis](https://github.com/SimonYHB/iOS-Framework-Analysis) 的第二遍，今年计划完成10个优秀第三方源码解读，欢迎 star 和笔者一起解读这些优秀框架的背后思想。该篇详细的源码注释已上传 [Aspects源码注释](https://github.com/SimonYHB/iOS-Framework-Analysis/tree/master/framework/Aspects)，如有需要请自取，若有什么不足之处，敬请告知  🐝🐝。

# 前言

AOP(Aspect-oriented programming)  也称之为 “面向切面编程”， 是一种通过预编译方式和运行期动态代理实现程序功能的统一维护的一种技术，通俗点将就是类似切片的方式，统一注入代码片段而不需要修改原有代码逻辑，相比于继承等方式，代码的耦合度更低。在java的Spring框架中应用广泛，而在iOS最火的AOP框架非 [Aspects](https://github.com/steipete/Aspects) 莫属。

# 初识Aspects

Aspects 是一个轻量级的 AOP框架，提供了实例和类方法对类中方法进行 Hook，可在原先方法 运行前/运行中/运行后 插入自定义的代码片段。其原理是把所有的方法调用指向 `_objc_msgForward` ，并处理原方法的参数列表和返回值，最后修改 `forwardInvocation` 方法使用 NSInvocation 去动态调用。相比于直接使用 `Method Swizzling` 交换原方法和新方法的 IMP 指针，Aspects 在内部做了更多的安全处理，使用起来更加可靠。

关于使用 Method Swizzling 存在的问题可查看 [iOS 界的毒瘤：Method Swizzle](https://juejin.im/entry/5a1fceddf265da43310d9985#menu_index_10)。

## 申明

```objective-c
#import <Foundation/Foundation.h>

typedef NS_OPTIONS(NSUInteger, AspectOptions) {
    AspectPositionAfter   = 0,            /// Called after the original implementation (default)
    AspectPositionInstead = 1,            /// Will replace the original implementation.
    AspectPositionBefore  = 2,            /// Called before the original implementation.
    
    AspectOptionAutomaticRemoval = 1 << 3 /// Will remove the hook after the first execution.
};

/// Opaque Aspect Token that allows to deregister the hook.
/// 用于注销Hook
@protocol AspectToken <NSObject>

/// Deregisters an aspect.
/// @return YES if deregistration is successful, otherwise NO.
- (BOOL)remove;

@end

/// 主要是所Hook方法的信息，用于校验block兼容性，后续触发block时会作为block的首个参数
@protocol AspectInfo <NSObject>

/// The instance that is currently hooked.
- (id)instance;

/// The original invocation of the hooked method.
- (NSInvocation *)originalInvocation;

/// All method arguments, boxed. This is lazily evaluated.
- (NSArray *)arguments;

@end

/// Aspects利用消息转发机制l来Hook消息，是存在性能开销的，不要在频繁调用的方法里去使用Aspects，主要用在view/controller的代码中
@interface NSObject (Aspects)


/// 在调用指定类的某个方法之前/过程中/之后执行一段block代码
/// block的第一个参数固定为id<AspectInfo>`, 所以要Hook的方法如果有参数，则第一个参数必须为对象，否则在比对签名时或校验不过
+ (id<AspectToken>)aspect_hookSelector:(SEL)selector
                           withOptions:(AspectOptions)options
                            usingBlock:(id)block
                                 error:(NSError **)error;

/// Adds a block of code before/instead/after the current `selector` for a specific instance.
- (id<AspectToken>)aspect_hookSelector:(SEL)selector
                           withOptions:(AspectOptions)options
                            usingBlock:(id)block
                                 error:(NSError **)error;

@end


typedef NS_ENUM(NSUInteger, AspectErrorCode) {
    AspectErrorSelectorBlacklisted,                   /// Selectors like release, retain, autorelease are blacklisted.
    AspectErrorDoesNotRespondToSelector,              /// Selector could not be found.
    AspectErrorSelectorDeallocPosition,               /// When hooking dealloc, only AspectPositionBefore is allowed.
    AspectErrorSelectorAlreadyHookedInClassHierarchy, /// Statically hooking the same method in subclasses is not allowed.
    AspectErrorFailedToAllocateClassPair,             /// The runtime failed creating a class pair.
    AspectErrorMissingBlockSignature,                 /// The block misses compile time signature info and can't be called.
    AspectErrorIncompatibleBlockSignature,            /// The block signature does not match the method or is too large.

    AspectErrorRemoveObjectAlreadyDeallocated = 100   /// (for removing) The object hooked is already deallocated.
};

extern NSString *const AspectErrorDomain;

```

使用方式比较简单，其创建 NSObject 的分类写入 Aspects 的相关方法，分别为类对象和实例对象提供调用方法，在需要 Hook 的地方调用即可。

另外分别定义了 `AspectToken` 和 `AspectInfo` 两个协议，AspectToken 实现了移除方法，AspectInfo 记录了原方法的信息，作为 `block` 的一个参数返回给使用者。

# 源码解读

## 内部定义

### AspectInfo

### AspectIdentifier

### AspectsContainer

### AspectTracker

### AspectBlockRef



## 调用流程

两个 API 的内部都是调用 `aspect_add` 函数，我们直接从该函数入手，看作者是如何设计实现的。



# 总结

## About Me



