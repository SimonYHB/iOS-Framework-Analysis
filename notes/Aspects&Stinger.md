AOP(Aspect-oriented programming) 也称之为 “面向切面编程”, 是一种通过预编译方式和运行期动态代理实现程序功能的统一维护的一种技术。在java的Spring框架中应用广泛。近几年在iOS最火的AOP框架非[Aspects]()莫属。19年年底饿了么团队开源了内部AOP库[Stinger]()，并宣称速度是Aspects的几倍，本文将对这两个新老框架进行详细解读及比较。  
该文章是iOS框架解读专栏的开篇，今年将会持续解读优秀的iOS框架，欢迎关注。

# Aspects
整个Aspects工程只有两个文件，实际会用到的方法有两个，分别是对类对象和实例对象进行拦截。 
```
/// 在调用指定类的某个方法之前/过程中/之后执行一段block代码
+ (id<AspectToken>)aspect_hookSelector:(SEL)selector
                           withOptions:(AspectOptions)options
                            usingBlock:(id)block
                                 error:(NSError **)error;

/// 在调用指定对象的某个方法之前/过程中/之后执行一段block代码
- (id<AspectToken>)aspect_hookSelector:(SEL)selector
                           withOptions:(AspectOptions)options
                            usingBlock:(id)block
                                 error:(NSError **)error;
```
## 整体流程

虽然类对象和实例对象的调用方法是分开的，但具体实现是放在一起处理的，先略过具体的实现，主要处理逻辑如下。

- Aspects

   - 判断方法是否允许Hook

      - 将方法转成标示记录到指定容器中(方便后续统一移除)

         - 类对象处理

           	- 修改

         - 实例对象处理

           	- 修改

           ​

## 具体实现
### AspectInfo<AspectInfo>
```
/// The AspectInfo protocol is the first parameter of our block syntax.
/// 主要是所Hook方法的信息，用于校验block兼容性，后续触发block时会作为block的首个参数
@protocol AspectInfo <NSObject>
/// The instance that is currently hooked.
- (id)instance;
/// The original invocation of the hooked method.
- (NSInvocation *)originalInvocation;
/// All method arguments, boxed. This is lazily evaluated.
- (NSArray *)arguments;
@end

//  Aspecth的环境，包含被Hook的实例、调用方法和参数
//  遵守AspectInfo协议
@interface AspectInfo : NSObject <AspectInfo>
- (id)initWithInstance:(__unsafe_unretained id)instance invocation:(NSInvocation *)invocation;
@property (nonatomic, unsafe_unretained, readonly) id instance;
@property (nonatomic, strong, readonly) NSArray *arguments;
@property (nonatomic, strong, readonly) NSInvocation *originalInvocation;
@end
```
### AspectIdentifier<AspectToken>
### AspectsContainer
### AspectTracker
### 全局变量
Aspects内部有定义一些全局变量和常量。   

- `static NSMutableDictionary *swizzledClassesDict; `    
swizzledClassesDict是一个MutableDictionary，内部每个元素key为Class，value为AspectTracker，记录所有被Aspects混写过的Class的继承链追踪记录信息，用于判断某个方法是否在类的继承链中有无被混写过。

```
static NSMutableDictionary *aspect_getSwizzledClassesDict() {
    //  用于记录所有被混写过的类 <Class : AspectTracker *>
    static NSMutableDictionary *swizzledClassesDict;
    static dispatch_once_t pred;
    dispatch_once(&pred, ^{
        swizzledClassesDict = [NSMutableDictionary new];
    });
    return swizzledClassesDict;
}
```
- `static NSMutableSet *swizzledClasses;`  
swizzledClasses是一个Set集合，内部存放的是被改写过`forwardInvocation:`方法的类名。

```
static void _aspect_modifySwizzledClasses(void (^block)(NSMutableSet *swizzledClasses)) {
    //  用于记录所有被混写过的类名，(className)
    static NSMutableSet *swizzledClasses;
    static dispatch_once_t pred;
    dispatch_once(&pred, ^{
        swizzledClasses = [NSMutableSet new];
    });
    @synchronized(swizzledClasses) {
        block(swizzledClasses);
    }
}
```
- 常量: AspectsSubclassSuffix、AspectsMessagePrefix、AspectsForwardInvocationSelectorName等，用于内部规范命名。

```
static NSString *const AspectsSubclassSuffix = @"_Aspects_";
static NSString *const AspectsMessagePrefix = @"aspects_";
static NSString *const AspectsForwardInvocationSelectorName = @"__aspects_forwardInvocation:";
```


### 关键方法解析
- `aspect_hookClass`
- `aspect_cleanupHookedClassAndSelector`
# Stinger
## 整体流程
## 具体实现


Stinger

https://juejin.im/post/5df5dcbc6fb9a0166138ff23
https://juejin.im/post/5a601903f265da3e5537f405