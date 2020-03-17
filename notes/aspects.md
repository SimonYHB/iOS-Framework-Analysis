> 本篇是笔者解读源码项目 [iOS-Framework-Analysis](https://github.com/SimonYHB/iOS-Framework-Analysis) 的第二篇，今年计划完成10个优秀第三方框架解读，欢迎 star 和笔者一起解读这些优秀框架的背后思想。该篇详细的源码注释已上传 [Aspects源码注释](https://github.com/SimonYHB/iOS-Framework-Analysis/tree/master/framework/Aspects)，如有需要请自取，若有什么不足之处，敬请告知  🐝🐝。

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

```objective-c
@interface AspectInfo : NSObject <AspectInfo>
- (id)initWithInstance:(__unsafe_unretained id)instance invocation:(NSInvocation *)invocation;
@property (nonatomic, unsafe_unretained, readonly) id instance;
@property (nonatomic, strong, readonly) NSArray *arguments;
@property (nonatomic, strong, readonly) NSInvocation *originalInvocation;
@end
```

Aspects 对象的环境，包含被 Hook 的实例、调用方法和参数，并遵守AspectInfo 协议。

### AspectIdentifier

```objective-c
@interface AspectIdentifier : NSObject
+ (instancetype)identifierWithSelector:(SEL)selector object:(id)object options:(AspectOptions)options block:(id)block error:(NSError **)error;
- (BOOL)invokeWithInfo:(id<AspectInfo>)info;
@property (nonatomic, assign) SEL selector;
@property (nonatomic, strong) id block;
@property (nonatomic, strong) NSMethodSignature *blockSignature;
@property (nonatomic, weak) id object;
@property (nonatomic, assign) AspectOptions options;
@end
```

Aspect 标识，包含一次完整 Aspect 的所有内容，会作为block 第一个参数，内部实现了remove方法，需要使用时遵守 AspectToken 协议即可。

### AspectsContainer

```objective-c
@interface AspectsContainer : NSObject
- (void)addAspect:(AspectIdentifier *)aspect withOptions:(AspectOptions)injectPosition;
- (BOOL)removeAspect:(id)aspect;
- (BOOL)hasAspects;
@property (atomic, copy) NSArray *beforeAspects;
@property (atomic, copy) NSArray *insteadAspects;
@property (atomic, copy) NSArray *afterAspects;
@end
```

AspectsContainer 是一个对象或者类的所有的 Aspects 的容器，每次注入Aspects时会将其按照 option 里的时机放到对应数组中，方便后续的统一管理(例如移除)。

通过 `objc_setAssociatedObject` 给 NSObject 注 AspectsContainer 属性，内部含有三个数组，对应关系如下。

```objective-c
NSArray *beforeAspects -> AspectPositionBefore

NSArray *insteadAspects -> AspectPositionInstead

NSArray *afterAspects -> AspectPositionAfter
```

### AspectTracker

```objective-c
@interface AspectTracker : NSObject
- (id)initWithTrackedClass:(Class)trackedClass;
@property (nonatomic, strong) Class trackedClass;
@property (nonatomic, readonly) NSString *trackedClassName;
@property (nonatomic, strong) NSMutableSet *selectorNames;
//用于标记其所有子类有Hook的方法 示例：[HookingSelectorName: (AspectTracker1,AspectTracker2...)]
@property (nonatomic, strong) NSMutableDictionary *selectorNamesToSubclassTrackers;
- (void)addSubclassTracker:(AspectTracker *)subclassTracker hookingSelectorName:(NSString *)selectorName;
- (void)removeSubclassTracker:(AspectTracker *)subclassTracker hookingSelectorName:(NSString *)selectorName;
- (BOOL)subclassHasHookedSelectorName:(NSString *)selectorName;
- (NSSet *)subclassTrackersHookingSelectorName:(NSString *)selectorName;
@end
```

每个被 Hook 过类都有一个对应 AspectTracker，以 `<Class : AspectTracker *>` 形式存储在 swizzledClassesDict 字典中，用于追踪记录类中 Hook 的方法。

### AspectBlockRef

```objective-c
typedef struct _AspectBlock {
	__unused Class isa;
	AspectBlockFlags flags;
	__unused int reserved;
	void (__unused *invoke)(struct _AspectBlock *block, ...);
	struct {
		unsigned long int reserved;
		unsigned long int size;
		// requires AspectBlockFlagsHasCopyDisposeHelpers
		void (*copy)(void *dst, const void *src);
		void (*dispose)(const void *);
		// requires AspectBlockFlagsHasSignature
		const char *signature;
		const char *layout;
	} *descriptor;
	// imported variables
} *AspectBlockRef;
```

内部定义的 block 结构体，用于转换外部 block ，与下面 block 源码定义很相似。

```c
 // 从block源码(libclosure)可知
 struct Block_layout {
 void *isa;
 int flags;
 int reserved;
 void (*invoke)(void *, ...);
 struct Block_descriptor *descriptor;

};
struct Block_descriptor {
    unsigned long int reserved;
    unsigned long int size;
    void (*copy)(void *dst, void *src);
    void (*dispose)(void *);
};
 // Values for Block_layout->flags to describe block objects
 enum {
 BLOCK_DEALLOCATING =      (0x0001),  // runtime
 BLOCK_REFCOUNT_MASK =     (0xfffe),  // runtime
 BLOCK_NEEDS_FREE =        (1 << 24), // runtime
 BLOCK_HAS_COPY_DISPOSE =  (1 << 25), // compiler
 BLOCK_HAS_CTOR =          (1 << 26), // compiler: helpers have C++ code
 BLOCK_IS_GC =             (1 << 27), // runtime
 BLOCK_IS_GLOBAL =         (1 << 28), // compiler
 BLOCK_USE_STRET =         (1 << 29), // compiler: undefined if !BLOCK_HAS_SIGNATURE
 BLOCK_HAS_SIGNATURE  =    (1 << 30), // compiler
 BLOCK_HAS_EXTENDED_LAYOUT=(1 << 31)  // compiler
 };
```





## 调用流程

两个 API 的内部都是调用 `aspect_add` 函数，我们直接从该函数入手，看作者是如何设计实现的。

```objective-c
static id aspect_add(id self, SEL selector, AspectOptions options, id block, NSError **error) {
    NSCParameterAssert(self);
    NSCParameterAssert(selector);
    NSCParameterAssert(block);

    __block AspectIdentifier *identifier = nil;
    aspect_performLocked(^{
        //- 判断要混写的方法是否在白名单中
        if (aspect_isSelectorAllowedAndTrack(self, selector, options, error)) {
            //- 获取混写方法容器
            AspectsContainer *aspectContainer = aspect_getContainerForObject(self, selector);
            //- 创建方法标示
            identifier = [AspectIdentifier identifierWithSelector:selector object:self options:options block:block error:error];
            if (identifier) {
                //- 根据标示将方法放在对应容器中
                [aspectContainer addAspect:identifier withOptions:options];

                // Modify the class to allow message interception.
                //  **关键：真正实现Aspect的方法**
                aspect_prepareClassAndHookSelector(self, selector, error);
            }
        }
    });
    return identifier;
}
```

我们先用一张流程图画下都做了些什么事情。

![aspects_0](/Users/yehuangbin/Desktop/github/iOS-Framework-Analysis/notes/images/aspects_0.jpg)

### 前置准备步骤

为了实现 Hook 注入，需要先做些准备工作，包括：

- 校验当前方法是否可以被 Hook，例如 retain、release、 forwardInvocation 等方法都是禁止被 Hook 的。
- 获取类中的 AspectsContainer 容器
- 将方法信息等封装成 AspectIdentifier，其中有比较严格的参数兼容判断，具体可看 `aspect_isCompatibleBlockSignature` 函数
- 将 AspectIdentifier 放入对应容器中

实现都比较易懂，这里就不累述了，详细可看 [Aspects源码注释](https://github.com/SimonYHB/iOS-Framework-Analysis/tree/master/framework/Aspects)。

### 关键实现aspect_prepareClassAndHookSelector

```objective-c
static void aspect_prepareClassAndHookSelector(NSObject *self, SEL selector, NSError **error) {
    NSCParameterAssert(selector);
    //- 传入self得到其指向的类
    //- 如果是类对象则Hook其forwardInvocation方法,将Container内的方法注入进去，在将class/metaClass返回
    //- 如果是示例对象，则通过动态创建子类的方式返回新创建的子类
    Class klass = aspect_hookClass(self, error);
    Method targetMethod = class_getInstanceMethod(klass, selector);
    IMP targetMethodIMP = method_getImplementation(targetMethod);
    //- 判断方法是否已经是走消息转发的形式，若不是则对其进行处理。
    if (!aspect_isMsgForwardIMP(targetMethodIMP)) {
        // Make a method alias for the existing method implementation, it not already copied.
        const char *typeEncoding = method_getTypeEncoding(targetMethod);
        //- 创建新的方法aspects_xxxx，方法的实现为原方法的实现，目的是保存原来方法的实现
        SEL aliasSelector = aspect_aliasForSelector(selector);
        if (![klass instancesRespondToSelector:aliasSelector]) {
            __unused BOOL addedAlias = class_addMethod(klass, aliasSelector, method_getImplementation(targetMethod), typeEncoding);
            NSCAssert(addedAlias, @"Original implementation for %@ is already copied to %@ on %@", NSStringFromSelector(selector), NSStringFromSelector(aliasSelector), klass);
        }
        //- 修改原方法的实现，将其替换为_objc_msgForward或_objc_msgForward_stret形式触发,从而使调用时能进入消息转发机制forwardInvocation
        // We use forwardInvocation to hook in.
        class_replaceMethod(klass, selector, aspect_getMsgForwardIMP(self, selector), typeEncoding);
        AspectLog(@"Aspects: Installed hook for -[%@ %@].", klass, NSStringFromSelector(selector));
    }
}
```



首先通过 `aspect_hookClass` 获取目标类，并替换 `forwardInvocation`方法注入 Hook 代码，然后将原方法的实现替换为 _objc_msgForward 或 _objc_msgForward_stret 形式触发，从而使调用时能进入消息转发机制调用 forwardInvocation。

### 获取目标类aspect_hookClass

```objective-c
static Class aspect_hookClass(NSObject *self, NSError **error) {
    NSCParameterAssert(self);
  
	Class statedClass = self.class;
	Class baseClass = object_getClass(self);

	NSString *className = NSStringFromClass(baseClass);

    //  判断是否已子类化过(类后缀为_Aspects_)
	if ([className hasSuffix:AspectsSubclassSuffix]) {
		return baseClass;

        //  若self是类对象或元类对象，则混写self(替换forwardInvocation方法)
	}else if (class_isMetaClass(baseClass)) {
        return aspect_swizzleClassInPlace((Class)self);
        //  statedClass！=baseClass，且不满足上述两个条件，则说明是KVO模式下的实例对象，要混写其metaClass
	}else if (statedClass != baseClass) {
        return aspect_swizzleClassInPlace(baseClass);
	}

    //  上述情况都不满足，则说明是实例对象
    //  采用动态创建子类向其注入方法，最后替换实例对象的isa指针使其指向新创建的子类来实现Aspects
    
    //  拼接_Aspects_后缀成新类名
	const char *subclassName = [className stringByAppendingString:AspectsSubclassSuffix].UTF8String;
    //  尝试用新类名获取类
	Class subclass = objc_getClass(subclassName);

	if (subclass == nil) {
        //  创建一个新类，并将原来的类作为其父类
		subclass = objc_allocateClassPair(baseClass, subclassName, 0);
		if (subclass == nil) {
            NSString *errrorDesc = [NSString stringWithFormat:@"objc_allocateClassPair failed to allocate class %s.", subclassName];
            AspectError(AspectErrorFailedToAllocateClassPair, errrorDesc);
            return nil;
        }
        //  改写subclass的forwardInvocation方法，插入Aspects
		aspect_swizzleForwardInvocation(subclass);
        //  改写subclass的.class方法，使其返回self.class
		aspect_hookedGetClass(subclass, statedClass);
        //  改写subclass.isa的.class方法，使其返回self.class
		aspect_hookedGetClass(object_getClass(subclass), statedClass);
        //  注册子类
		objc_registerClassPair(subclass);
	}
    //  更改isa指针
	object_setClass(self, subclass);
	return subclass;
}
```

`aspect_hookClass` 分别对实例对象和类对象做了不同处理。首先通过 `self.class` 和 `objc_getClass(self)` 的值来判断当前对象的环境，分为四种场景，分别是 子类化过的实例对象、类对象和元类对象 、 KVO模式下的实例对象和实例对象。对于子类化过的实例对象直接返回其类即可；类对象、元类对象和 KVO模式下的实例对象调用 `aspect_swizzleClassInPlace` 替换 `forwardInvocation` 的实现；若是实例对象，则创建以 `_Aspects_` 结尾的子类，再替换 ``forwardInvocation`` 的实现和实例对象 `isa` 指针。

关于 `self.class` 和 `objc_getClass(self)` 这里稍微补充下：

- self.class: 当self是实例对象的时候，返回的是类对象，否则则返回自身 。  

- object_getClass: 获得的是 isa 的指针。  

- 当 self 是实例对象时，self.class 和 object_getClass(self) 相同，都是指向其类，当 self 为类对象时，self.class 是自身类，object_getClass(self) 则是其 metaClass。

### 真正调用APECTS_ARE_BEING_CALLED

```objective-c
//  交换后的__aspects_forwardInvocation:方法实现
static void __ASPECTS_ARE_BEING_CALLED__(__unsafe_unretained NSObject *self, SEL selector, NSInvocation *invocation) {
    NSCParameterAssert(self);
    NSCParameterAssert(invocation);
    SEL originalSelector = invocation.selector;
	SEL aliasSelector = aspect_aliasForSelector(invocation.selector);
    invocation.selector = aliasSelector;
    AspectsContainer *objectContainer = objc_getAssociatedObject(self, aliasSelector);
    AspectsContainer *classContainer = aspect_getContainerForClass(object_getClass(self), aliasSelector);
    AspectInfo *info = [[AspectInfo alloc] initWithInstance:self invocation:invocation];
    NSArray *aspectsToRemove = nil;

    // Before hooks.
    aspect_invoke(classContainer.beforeAspects, info);
    aspect_invoke(objectContainer.beforeAspects, info);

    // Instead hooks.
    BOOL respondsToAlias = YES;
    if (objectContainer.insteadAspects.count || classContainer.insteadAspects.count) {
        aspect_invoke(classContainer.insteadAspects, info);
        aspect_invoke(objectContainer.insteadAspects, info);
    }else {
        Class klass = object_getClass(invocation.target);
        do {
            if ((respondsToAlias = [klass instancesRespondToSelector:aliasSelector])) {
                [invocation invoke];
                break;
            }
        }while (!respondsToAlias && (klass = class_getSuperclass(klass)));
    }

    // After hooks.
    aspect_invoke(classContainer.afterAspects, info);
    aspect_invoke(objectContainer.afterAspects, info);

    // If no hooks are installed, call original implementation (usually to throw an exception)
    if (!respondsToAlias) {
        invocation.selector = originalSelector;
        SEL originalForwardInvocationSEL = NSSelectorFromString(AspectsForwardInvocationSelectorName);
        if ([self respondsToSelector:originalForwardInvocationSEL]) {
            ((void( *)(id, SEL, NSInvocation *))objc_msgSend)(self, originalForwardInvocationSEL, invocation);
        }else {
            [self doesNotRecognizeSelector:invocation.selector];
        }
    }

    // Remove any hooks that are queued for deregistration.
    [aspectsToRemove makeObjectsPerformSelector:@selector(remove)];
}
```

做完前面步骤后，当调用目标方法时，就是走到替换的 `__ASPECTS_ARE_BEING_CALLED__` 方法中，按调用时机从 AspectsContainer 获取 Aspects 注入。

### 移除aspect_remove

移除的逻辑比较清晰，这里就用图描述下具体都做了什么，配合代码注释使用更佳。

![aspects_1](/Users/yehuangbin/Desktop/github/iOS-Framework-Analysis/notes/images/aspects_1.jpg)

# 总结

`Aspects` 无论从功能性还是安全性上都可以称得上是非常优秀的 AOP 库，调用接口简单明了，内部考虑了很多异常场景，每个类的功能职责拆分得很细，非常推荐读者根据 [Aspects源码注释](https://github.com/SimonYHB/iOS-Framework-Analysis/tree/master/framework/Aspects) 再细看一遍。

至此，今年 [iOS优秀开源框架解析](https://github.com/SimonYHB/iOS-Framework-Analysis) 的第二篇结束 🎉🎉。

