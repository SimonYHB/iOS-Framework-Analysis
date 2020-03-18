
# Stinger
替换原方法的 IMP 为一个和原方法参数相同(type encoding)的方法的函数指针，作为壳，处理消息时，在这个壳内部拿到所有参数，最后通过函数指针直接执行“前”、“原始/替换”，“后”的多个代码块。

libffi 实现动态 C函数调用。



## 整体流程
## 具体实现


Stinger

https://juejin.im/post/5df5dcbc6fb9a0166138ff23
https://juejin.im/post/5a601903f265da3e5537f405
