# mongo_match_plugin

## 什么是mongo_match_plugin

mongo_match_plugin 是为了配合eosforce链上sys.match撮合合约而编写的将match中的挂单和成交相关表格写入mongo数据库的插件

## 编译mongo_match_plugin

1.将mongo_match_plugin 下载到eosforce的代码文件中
```
cd /usr/local/eosforce/plugins/
git clone https://github.com/xuyp1991/mongo_match_plugin.git
```

2.在CMakeList.txt文件中添加mongo_match_plugin

```
(1)修改 /usr/local/eosforce/plugins/CMakeLists.txt:
add_subdirectory(mongo_match_plugin)

(2)修改 /usr/local/eosforce/programs/nodeos/CMakeLists.txt:
target_link_libraries( nodeos PRIVATE -Wl,${whole_archive_flag} mongo_match_plugin -Wl,${no_whole_archive_flag} )
```

## 使用mongo_match_plugin

在config.ini上面添加相关配置
```
plugin = eosio::mongo_match_plugin
match_mongodb-uri = mongodb://127.0.0.1:27017/match
```