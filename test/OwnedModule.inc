module OwnedModule;

string title := concat("module-", "title");
ModuleItem item := makeModuleItem();
ModuleItem[] items := makeModuleItems();

sub makeModuleItem() returns ModuleItem {
  ModuleItem result := {value := 7, label := "module-item"};
  return result;
}

sub makeModuleItems() returns ModuleItem[] {
  ModuleItem[] result;
  append(result, makeModuleItem());
  return result;
}
