type k = string;

type boxT('v) = {
  key: k,
  value: 'v,
  id: int,
  prev: option(boxT('v)),
};

type t('v) =
  | Index(int, array(t('v)))
  | Collision(array(boxT('v)), int)
  | Leaf(boxT('v), int)
  | Empty;

/*-- Helpers -------------------------------------*/

let mask = (x: int, pos: int) => 1 lsl (x lsr (pos * 5) land 31);

let smi = (x: int) => x lsr 1 land 0x40000000 lor (x land 0xbfffffff);

let hash = (x: string) => {
  let length = String.length(x);
  let rec explode = (h, i) =>
    if (i < length) {
      let h2 = h lsl 5 + h + int_of_char(String.unsafe_get(x, i));
      explode(h2, i + 1);
    } else {
      h;
    };
  smi(explode(5381, 0));
};

let hammingWeight = (x: int) => {
  let x = x - x lsl 1 land 0x55555555;
  let x = x land 0x33333333 + x lsl 2 land 0x33333333;
  let x = (x + x lsl 4) land 0x0f0f0f0f;
  let x = x + x lsl 8;
  let x = x + x lsl 16;
  x land 0x7f;
};

let indexBit = (x: int, pos: int) => hammingWeight(x land (pos - 1));

/*-- Main methods -------------------------------------*/

let make = () => Index(0, [||]);

let get = (map: t('v), k: k): option('v) => {
  let code = hash(k);

  let rec traverse = (node: t('a), depth: int) =>
    switch (node) {
    | Index(bitmap, contents) =>
      let pos = mask(code, depth);
      let has = bitmap land pos;
      if (has !== 0) {
        let index = indexBit(bitmap, pos);
        let child = Js.Array.unsafe_get(contents, index);
        traverse(child, depth + 1);
      } else {
        None;
      };

    | Collision(bucket, _) =>
      switch (Js.Array.find(({key}) => key === k, bucket)) {
      | Some({value}) => Some(value)
      | None => None
      }

    | Leaf({key, value}, _) when key === k => Some(value)

    | Empty
    | Leaf(_) => None
    };

  traverse(map, 0);
};

let rec make_index = (code_a, code_b, a, b, depth) => {
  let pos_a = mask(code_a, depth);
  let pos_b = mask(code_b, depth);
  let bitmap = pos_a lor pos_b;
  let contents =
    if (pos_a === pos_b) {
      [|make_index(code_a, code_b, a, b, depth + 1)|];
    } else {
      indexBit(bitmap, pos_a) !== 0 ? [|b, a|] : [|a, b|];
    };

  Index(bitmap, contents);
};

let setOptimistic = (map: t('v), k: k, v: 'v, id: int): t('v) => {
  let code = hash(k);
  let optimistic = id === 0;
  let vbox = {key: k, value: v, id, prev: None};

  let rec traverse = (node, depth) =>
    switch (node) {
    | Index(bitmap, contents) =>
      let pos = mask(code, depth);
      let has = bitmap land pos;
      let bitmap = bitmap lor pos;
      let index = indexBit(bitmap, pos);

      let contents =
        if (has !== 0) {
          let node =
            traverse(Js.Array.unsafe_get(contents, index), depth + 1);
          let contents = Js.Array.copy(contents);
          Array.unsafe_set(contents, index, node);
          contents;
        } else {
          Js.Array.spliceInPlace(
            ~pos=index,
            ~remove=0,
            ~add=[|Leaf(vbox, code)|],
            Js.Array.copy(contents),
          );
        };
      Index(bitmap, contents);

    | Leaf({key} as box, _) when key === k && optimistic =>
      Leaf({...vbox, prev: Some(box)}, code)

    | Leaf({key}, _) when key === k => Leaf(vbox, code)

    | Leaf(box, c) when c === code => Collision([|vbox, box|], code)

    | Collision(bucket, c) when c === code =>
      let index = Js.Array.findIndex(({key}) => key === k, bucket);
      if (index > (-1)) {
        let prev = Js.Array.unsafe_get(bucket, index);
        let bucket = Js.Array.copy(bucket);
        Js.Array.unsafe_set(
          bucket,
          index,
          optimistic ? {...vbox, prev: Some(prev)} : vbox,
        );
        Collision(bucket, code);
      } else {
        Collision(Js.Array.concat(bucket, [|vbox|]), code);
      };

    | Leaf(_, c) as n
    | Collision(_, c) as n =>
      make_index(c, code, n, Leaf(vbox, code), depth)

    | Empty => Leaf(vbox, code)
    };

  traverse(map, 0);
};

let set = (map, k, v) => setOptimistic(map, k, v, 0);

let delete = (map: t('v), k: k): t('v) => {
  let code = hash(k);

  let rec traverse = (node, depth) =>
    switch (node) {
    | Index(bitmap, contents) =>
      let pos = mask(code, depth);
      let has = bitmap land pos;
      let index = indexBit(bitmap, pos);
      if (has !== 0) {
        let node = traverse(Js.Array.unsafe_get(contents, index), depth + 1);
        if (node === Empty) {
          let bitmap = bitmap lxor pos;
          if (bitmap === 0) {
            Empty;
          } else {
            let contents =
              Js.Array.removeFromInPlace(
                ~pos=index,
                Js.Array.copy(contents),
              );
            Index(bitmap, contents);
          };
        } else {
          let contents = Js.Array.copy(contents);
          Js.Array.unsafe_set(contents, index, node);
          Index(bitmap, contents);
        };
      } else {
        node;
      };

    | Leaf({key}, _) when key === k => Empty
    | Leaf(_) => node

    | Collision(bucket, c) when c === code =>
      let bucket = Js.Array.filter(({key}) => key === k, bucket);
      switch (bucket) {
      | [||] => Empty
      | [|box|] => Leaf(box, code)
      | _ => Collision(bucket, code)
      };
    | Collision(_) => node

    | Empty => Empty
    };

  traverse(map, 0);
};

let clear_box = (box: boxT('a), optid: int) => {
  let rec filter = (x: option(boxT('a))) =>
    switch (x) {
    | Some({id, prev}) when id === optid => filter(prev)
    | Some(b) => Some({...b, prev: filter(b.prev)})
    | None => None
    };
  filter(Some(box));
};

let clearOptimistic = (map: t('v), optid: int): t('v) => {
  let rec traverse = (node, depth) =>
    switch (node) {
    | Leaf({id} as box, code) when id !== 0 =>
      switch (clear_box(box, optid)) {
      | Some(box) => Leaf(box, code)
      | None => Empty
      }

    | Leaf(_) as n => n

    | Index(bitmap, contents) =>
      let hasContent = ref(false);
      let contents =
        Js.Array.map(
          node => {
            let node = traverse(node, depth + 1);
            if (node !== Empty) {
              hasContent := true;
            };
            node;
          },
          contents,
        );
      hasContent^ ? Index(bitmap, contents) : Empty;

    | Collision(bucket, code) =>
      let bucket =
        Js.Array.reduce(
          (acc, box) =>
            if (box.id !== 0) {
              switch (clear_box(box, optid)) {
              | Some(box) =>
                ignore(Js.Array.push(box, acc));
                acc;
              | None => acc
              };
            } else {
              ignore(Js.Array.push(box, acc));
              acc;
            },
          [||],
          bucket,
        );
      switch (bucket) {
      | [||] => Empty
      | [|box|] => Leaf(box, code)
      | _ => Collision(bucket, code)
      };

    | Empty => Empty
    };

  traverse(map, 0);
};