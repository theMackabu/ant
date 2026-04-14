class Widget {
  constructor(key) {
    this.key = key;
  }
  canUpdate(other) {
    return this.constructor === other.constructor && this.key === other.key;
  }
}

let BUILD_OWNER = null;

class Element {
  constructor(widget) {
    this.widget = widget;
    this.parent = void 0;
    this._children = [];
    this._dirty = false;
    this._mounted = false;
  }
  get dirty() {
    return this._dirty;
  }
  addChild(child) {
    child.parent = this;
    this._children.push(child);
  }
  removeChild(child) {
    const i = this._children.indexOf(child);
    if (i >= 0) this._children.splice(i, 1);
    child.parent = void 0;
  }
  markMounted() {
    this._mounted = true;
  }
  unmount() {
    this._mounted = false;
    this._dirty = false;
  }
  markNeedsRebuild() {
    if (!this._mounted) return;
    this._dirty = true;
    BUILD_OWNER.scheduleBuildFor(this);
  }
  markNeedsBuild() {
    this.markNeedsRebuild();
  }
  update(widget) {
    this.widget = widget;
  }
}

class BuildOwner {
  constructor() {
    this._dirtyElements = new Set();
  }
  scheduleBuildFor(el) {
    this._dirtyElements.add(el);
  }
  buildScopes() {
    const batch = Array.from(this._dirtyElements);
    this._dirtyElements.clear();
    for (const el of batch) {
      if (el.dirty) {
        el.performRebuild();
        el._dirty = false;
      }
    }
  }
}

class State {
  _mounted = false;
  _mount(widget, context) {
    this.widget = widget;
    this.context = context;
    this._mounted = true;
    this.initState?.();
  }
  _update(widget) {
    const oldWidget = this.widget;
    this.widget = widget;
    this.didUpdateWidget?.(oldWidget);
  }
  _unmount() {
    this._mounted = false;
    this.dispose?.();
  }
  setState(fn) {
    if (!this._mounted) throw new Error("setState() called after dispose()");
    if (fn) fn();
    this.context.element.markNeedsBuild();
  }
}

class BuildContext {
  constructor(element, widget) {
    this.element = element;
    this.widget = widget;
  }
}

class RenderObjectWidget extends Widget {
  createElement() {
    return new RenderObjectElement(this);
  }
}

class RenderObjectElement extends Element {
  mount() {
    this.renderObject = this.widget.createRenderObject();
    this.markMounted();
  }
  update(widget) {
    super.update(widget);
    widget.updateRenderObject(this.renderObject);
  }
  performRebuild() {}
}

class StatefulWidget extends Widget {
  createElement() {
    return new StatefulElement(this);
  }
}

class StatefulElement extends Element {
  mount() {
    this._context = new BuildContext(this, this.widget);
    this._state = this.widget.createState();
    this._state._mount(this.widget, this._context);
    this.rebuild();
    this.markMounted();
  }
  performRebuild() {
    this.rebuild();
  }
  rebuild() {
    const next = this._state.build(this._context);
    if (!this._child) {
      this._child = next.createElement();
      this.addChild(this._child);
      this._child.mount();
      return;
    }
    if (this._child.widget.canUpdate(next)) {
      this._child.update(next);
    } else {
      this._child.unmount();
      this.removeChild(this._child);
      this._child = next.createElement();
      this.addChild(this._child);
      this._child.mount();
    }
  }
}

class Label extends RenderObjectWidget {
  constructor(text) {
    super();
    this.text = text;
  }
  createRenderObject() {
    return { text: this.text };
  }
  updateRenderObject(ro) {
    ro.text = this.text;
    console.log("renderObject updated ->", JSON.stringify(ro.text));
  }
}

class CounterApp extends StatefulWidget {
  createState() {
    return new CounterState();
  }
}

class CounterState extends State {
  initState() {
    this.count = 0;
  }
  build() {
    console.log("build ->", this.count);
    return new Label("count:" + this.count);
  }
}

BUILD_OWNER = new BuildOwner();

const root = new CounterApp().createElement();
root.mount();

for (let i = 1; i <= 5; i++) {
  root._state.setState(() => {
    root._state.count = i;
  });
  BUILD_OWNER.buildScopes();
  console.log("visible now ->", root._child.renderObject.text);
}
