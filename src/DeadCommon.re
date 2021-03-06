/* Adapted from https://github.com/LexiFi/dead_code_analyzer */

// Turn on type analysis
let analyzeTypes = ref(true);

let analyzeExternals = true;

let debug = ref(false);

let removeDeadValuesWithSideEffects = false;

let reportUnderscore = false;

let reportTypesDeadOnlyInInterface = false;

let recursiveDebug = false;

let warnOnCircularDependencies = false;

module Name: {
  type t;
  let create: (~isInterface: bool=?, string) => t;
  let isUnderscore: t => bool;
  let startsWithUnderscore: t => bool;
  let toImplementation: t => t;
  let toInterface: t => t;
  let toString: t => string;
} = {
  type t = string;
  let create = (~isInterface=true, s) => isInterface ? s : "+" ++ s;
  let isInterface = s => s.[0] != '+';
  let isUnderscore = s => s == "_" || s == "+_";
  let startsWithUnderscore = s =>
    s |> String.length >= 2 && (s.[0] == '_' || s.[0] == '+' && s.[1] == '_');
  let toInterface = s =>
    isInterface(s) ? s : String.sub(s, 1, String.length(s) - 1);
  let toImplementation = s => isInterface(s) ? "+" ++ s : s;
  let toString = s => s;
};

let checkPrefix = prefix_ => {
  let prefix =
    Paths.projectRoot^ == ""
      ? prefix_ : Filename.concat(Paths.projectRoot^, prefix_);
  let prefixLen = prefix |> String.length;
  sourceDir =>
    String.length(sourceDir) >= prefixLen
    && String.sub(sourceDir, 0, prefixLen) == prefix;
};

let rec checkSub = (s1, s2, n) =>
  n <= 0 || s1.[n] == s2.[n] && checkSub(s1, s2, n - 1);
let fileIsImplementationOf = (s1, s2) => {
  let n1 = String.length(s1)
  and n2 = String.length(s2);
  n2 == n1 + 1 && checkSub(s1, s2, n1 - 1);
};

// Whitelist=prefix only report on source dirs with the given prefix
let whitelistSourceDir =
  lazy(
    {
      switch (Sys.getenv_opt("Whitelist")) {
      | None => (_sourceDir => true)
      | Some(prefix) => checkPrefix(prefix)
      };
    }
  );

let posInWhitelist = (pos: Lexing.position) => {
  pos.pos_fname |> Lazy.force(whitelistSourceDir);
};

// Blacklist=prefix don't report on source dirs with the given prefix
let blacklistSourceDir =
  lazy(
    {
      switch (Sys.getenv_opt("Blacklist")) {
      | None => (_sourceDir => false)
      | Some(prefix) => checkPrefix(prefix)
      };
    }
  );

let posInBlacklist = (pos: Lexing.position) => {
  pos.pos_fname |> Lazy.force(blacklistSourceDir);
};

let write = ref(false);

let deadAnnotation = "dead";
let liveAnnotation = "live";

/* Location printer: `filename:line: ' */
let posToString = (~printCol=true, ~shortFile=true, pos: Lexing.position) => {
  let file = pos.Lexing.pos_fname;
  let line = pos.Lexing.pos_lnum;
  let col = pos.Lexing.pos_cnum - pos.Lexing.pos_bol;
  (shortFile ? file |> Filename.basename : file)
  ++ ":"
  ++ string_of_int(line)
  ++ (printCol ? ":" ++ string_of_int(col) : ": ");
};

let posIsReason = Log_.posIsReason;

/********   ATTRIBUTES   ********/
module PosSet =
  Set.Make({
    type t = Lexing.position;
    let compare = compare;
  });

module PosHash = {
  include Hashtbl.Make({
    type t = Lexing.position;

    let hash = x => {
      let s = Filename.basename(x.Lexing.pos_fname);
      Hashtbl.hash((x.Lexing.pos_cnum, s));
    };

    let equal = (x: t, y) => x == y;
  });

  let findSet = (h, k) =>
    try(find(h, k)) {
    | Not_found => PosSet.empty
    };

  let addSet = (h, k, v) => {
    let set = findSet(h, k);
    replace(h, k, PosSet.add(v, set));
  };
};

module FileSet = Set.Make(String);

module FileHash = {
  include Hashtbl.Make({
    type t = string;

    let hash = (x: t) => Hashtbl.hash(x);

    let equal = (x: t, y) => x == y;
  });

  let findSet = (table, key) =>
    try(find(table, key)) {
    | Not_found => FileSet.empty
    };

  let addFile = (table, key) => {
    let set = findSet(table, key);
    replace(table, key, set);
  };

  let addSet = (table, key, value) => {
    let set = findSet(table, key);
    replace(table, key, FileSet.add(value, set));
  };
};

type path = list(Name.t);

type declKind =
  | RecordLabel
  | VariantCase
  | Value;

type decl = {
  declKind,
  path,
  pos: Lexing.position,
  posEnd: Lexing.position,
  posStart: Lexing.position,
  mutable resolved: bool,
  sideEffects: bool,
};

type decls = PosHash.t(decl);

let decls: decls = PosHash.create(256); /* all exported declarations */
let moduleDecls: Hashtbl.t(Name.t, PosSet.t) = Hashtbl.create(1); /* from module name to its decls */

let valueReferences: PosHash.t(PosSet.t) = PosHash.create(256); /* all value references */
let typeReferences: PosHash.t(PosSet.t) = PosHash.create(256); /* all type references */

let fileReferences: FileHash.t(FileSet.t) = FileHash.create(256); /* references across files */

let typeLabels: Hashtbl.t(string, Location.t) = Hashtbl.create(256); /* map from type items (record/variant label) paths and locations */

let currentSrc = ref("");
let currentModule = ref("");
let currentModuleName = ref("" |> Name.create);
let currentBindings = ref(PosSet.empty);
let lastBinding = ref(Location.none);
let getLastBinding = () => lastBinding^;
let maxValuePosEnd = ref(Lexing.dummy_pos); // max end position of a value reported dead
let liveNames = ref([]: list(string)); // names to be considered live values
let livePaths = ref([]: list(string)); // paths of files where all values are considered live

let declGetLoc = decl => {
  Location.loc_start: decl.posStart,
  loc_end: decl.posEnd,
  loc_ghost: false,
};

let getPosOfValue = (~moduleName, ~valueName) => {
  let lookup = name =>
    switch (Hashtbl.find_opt(moduleDecls, name)) {
    | Some(posSet) =>
      posSet
      |> PosSet.find_first_opt(pos =>
           switch (PosHash.find_opt(decls, pos)) {
           | Some({declKind: Value, path: [name, ..._]})
               when name == valueName =>
             true
           | _ => false
           }
         )
    | None => None
    };

  switch (lookup(moduleName |> Name.toInterface)) {
  | None => lookup(moduleName |> Name.toImplementation)
  | Some(x) => Some(x)
  };
};

let getDeclPositions = (~moduleName) => {
  switch (Hashtbl.find_opt(moduleDecls, moduleName)) {
  | Some(posSet) => posSet
  | None => PosSet.empty
  };
};

/* Keep track of the module path while traversing with Tast_mapper */
let currentModulePath: ref(path) = ref([]);

/********   HELPERS   ********/

let addValueReference =
    (~addFileReference, ~locFrom: Location.t, ~locTo: Location.t) => {
  let lastBinding = getLastBinding();
  let locFrom = lastBinding == Location.none ? locFrom : lastBinding;
  if (!locFrom.loc_ghost) {
    if (debug^) {
      Log_.item(
        "addValueReference %s --> %s@.",
        locFrom.loc_start |> posToString,
        locTo.loc_start |> posToString,
      );
    };
    PosHash.addSet(valueReferences, locTo.loc_start, locFrom.loc_start);
    if (addFileReference
        && !locTo.loc_ghost
        && !locFrom.loc_ghost
        && locFrom.loc_start.pos_fname != locTo.loc_start.pos_fname) {
      FileHash.addSet(
        fileReferences,
        locFrom.loc_start.pos_fname,
        locTo.loc_start.pos_fname,
      );
    };
  };
};

let iterFilesFromRootsToLeaves = iterFun => {
  /* For each file, the number of incoming references */
  let inverseReferences: Hashtbl.t(string, int) = Hashtbl.create(1);
  /* For each number of incoming references, the files */
  let referencesByNumber: Hashtbl.t(int, FileSet.t) = Hashtbl.create(1);

  let getNum = fileName =>
    try(Hashtbl.find(inverseReferences, fileName)) {
    | Not_found => 0
    };

  let getSet = num =>
    try(Hashtbl.find(referencesByNumber, num)) {
    | Not_found => FileSet.empty
    };

  let addIncomingEdge = fileName => {
    let oldNum = getNum(fileName);
    let newNum = oldNum + 1;
    let oldSetAtNum = getSet(oldNum);
    let newSetAtNum = FileSet.remove(fileName, oldSetAtNum);
    let oldSetAtNewNum = getSet(newNum);
    let newSetAtNewNum = FileSet.add(fileName, oldSetAtNewNum);
    Hashtbl.replace(inverseReferences, fileName, newNum);
    Hashtbl.replace(referencesByNumber, oldNum, newSetAtNum);
    Hashtbl.replace(referencesByNumber, newNum, newSetAtNewNum);
  };

  let removeIncomingEdge = fileName => {
    let oldNum = getNum(fileName);
    let newNum = oldNum - 1;
    let oldSetAtNum = getSet(oldNum);
    let newSetAtNum = FileSet.remove(fileName, oldSetAtNum);
    let oldSetAtNewNum = getSet(newNum);
    let newSetAtNewNum = FileSet.add(fileName, oldSetAtNewNum);
    Hashtbl.replace(inverseReferences, fileName, newNum);
    Hashtbl.replace(referencesByNumber, oldNum, newSetAtNum);
    Hashtbl.replace(referencesByNumber, newNum, newSetAtNewNum);
  };

  let isSourceFile = fileName => FileHash.mem(fileReferences, fileName);

  let addEdge = (fromFile, toFile) =>
    if (isSourceFile(fromFile)) {
      addIncomingEdge(toFile);
    };

  let removeEdge = (fromFile, toFile) =>
    if (isSourceFile(fromFile)) {
      removeIncomingEdge(toFile);
    };

  fileReferences
  |> FileHash.iter((fromFile, set) => {
       if (getNum(fromFile) == 0) {
         Hashtbl.replace(
           referencesByNumber,
           0,
           FileSet.add(fromFile, getSet(0)),
         );
       };
       set |> FileSet.iter(toFile => {addEdge(fromFile, toFile)});
     });

  while (getSet(0) != FileSet.empty) {
    let filesWithNoIncomingReferences = getSet(0);
    Hashtbl.remove(referencesByNumber, 0);
    filesWithNoIncomingReferences
    |> FileSet.iter(fileName => {
         iterFun(fileName);
         let references =
           try(FileHash.find(fileReferences, fileName)) {
           | Not_found => FileSet.empty
           };
         references |> FileSet.iter(toFile => removeEdge(fileName, toFile));
       });
  };
  // Process any remaining items in case of circular references
  referencesByNumber
  |> Hashtbl.iter((_num, set) =>
       if (FileSet.is_empty(set)) {
         ();
       } else {
         set
         |> FileSet.iter(fileName => {
              let pos = {...Lexing.dummy_pos, pos_fname: fileName};
              let loc = {...Location.none, loc_start: pos, loc_end: pos};
              if (warnOnCircularDependencies) {
                Log_.info(
                  ~loc, ~name="Warning Dead Analysis Cycle", (ppf, ()) =>
                  Format.fprintf(
                    ppf,
                    "Results for %s could be inaccurate because of circular references",
                    fileName,
                  )
                );
              };
              iterFun(fileName);
            });
       }
     );
};

/* Keep track of the location of values annotated @genType or @dead */
module ProcessDeadAnnotations = {
  type annotatedAs =
    | GenType
    | Dead
    | Live;

  let positionsAnnotated = PosHash.create(1);

  let isAnnotatedDead = pos =>
    PosHash.find_opt(positionsAnnotated, pos) == Some(Dead);

  let isAnnotatedGenTypeOrLive = pos =>
    switch (PosHash.find_opt(positionsAnnotated, pos)) {
    | Some(Live | GenType) => true
    | Some(Dead)
    | None => false
    };

  let isAnnotatedGenTypeOrDead = pos =>
    switch (PosHash.find_opt(positionsAnnotated, pos)) {
    | Some(Dead | GenType) => true
    | Some(Live)
    | None => false
    };

  let annotateGenType = (pos: Lexing.position) => {
    PosHash.replace(positionsAnnotated, pos, GenType);
  };

  let annotateDead = (pos: Lexing.position) => {
    PosHash.replace(positionsAnnotated, pos, Dead);
  };

  let annotateLive = (pos: Lexing.position) => {
    PosHash.replace(positionsAnnotated, pos, Live);
  };

  let processAttributes = (~doGenType, ~name, ~pos, attributes) => {
    let getPayloadFun = f => attributes |> Annotation.getAttributePayload(f);
    let getPayload = (x: string) =>
      attributes |> Annotation.getAttributePayload((==)(x));

    if (doGenType
        && getPayloadFun(Annotation.tagIsOneOfTheGenTypeAnnotations) != None) {
      pos |> annotateGenType;
    };

    if (getPayload(deadAnnotation) != None) {
      pos |> annotateDead;
    };

    let nameIsInLiveNamesOrPaths = () =>
      liveNames^
      |> List.mem(name)
      || {
        let fname = pos.pos_fname;
        let fnameLen = String.length(fname);
        livePaths^
        |> List.exists(prefix =>
             String.length(prefix) <= fnameLen
             && String.sub(fname, 0, String.length(prefix)) == prefix
           );
      };

    if (getPayload(liveAnnotation) != None || nameIsInLiveNamesOrPaths()) {
      pos |> annotateLive;
    };

    if (attributes |> Annotation.isOcamlSuppressDeadWarning) {
      pos |> annotateLive;
    };
  };

  let collectExportLocations = (~doGenType) => {
    let super = Tast_mapper.default;
    let currentlyDisableWarnings = ref(false);
    let value_binding =
        (
          self,
          {vb_attributes, vb_pat} as value_binding: Typedtree.value_binding,
        ) => {
      switch (vb_pat.pat_desc) {
      | Tpat_var(id, {loc: {loc_start: pos}}) =>
        if (currentlyDisableWarnings^) {
          pos |> annotateLive;
        };
        vb_attributes
        |> processAttributes(~doGenType, ~name=id |> Ident.name, ~pos);

      | _ => ()
      };
      super.value_binding(self, value_binding);
    };
    let type_kind = (self, typeKind: Typedtree.type_kind) => {
      switch (typeKind) {
      | Ttype_record(labelDeclarations) =>
        labelDeclarations
        |> List.iter(({ld_attributes, ld_loc}: Typedtree.label_declaration) =>
             ld_attributes
             |> processAttributes(~doGenType, ~name="", ~pos=ld_loc.loc_start)
           )
      | Ttype_variant(constructorDeclarations) =>
        constructorDeclarations
        |> List.iter(
             ({cd_attributes, cd_loc}: Typedtree.constructor_declaration) =>
             cd_attributes
             |> processAttributes(~doGenType, ~name="", ~pos=cd_loc.loc_start)
           )
      | _ => ()
      };
      super.type_kind(self, typeKind);
    };
    let value_description =
        (
          self,
          {val_attributes, val_id, val_val: {val_loc: {loc_start: pos}}} as value_description: Typedtree.value_description,
        ) => {
      if (currentlyDisableWarnings^) {
        pos |> annotateLive;
      };
      val_attributes
      |> processAttributes(~doGenType, ~name=val_id |> Ident.name, ~pos);
      super.value_description(self, value_description);
    };
    let structure_item = (self, item: Typedtree.structure_item) => {
      switch (item.str_desc) {
      | Tstr_attribute(attribute)
          when [attribute] |> Annotation.isOcamlSuppressDeadWarning =>
        currentlyDisableWarnings := true
      | _ => ()
      };
      super.structure_item(self, item);
    };
    let structure = (self, structure: Typedtree.structure) => {
      let oldDisableWarnings = currentlyDisableWarnings^;
      super.structure(self, structure) |> ignore;
      currentlyDisableWarnings := oldDisableWarnings;
      structure;
    };
    let signature_item = (self, item: Typedtree.signature_item) => {
      switch (item.sig_desc) {
      | Tsig_attribute(attribute)
          when [attribute] |> Annotation.isOcamlSuppressDeadWarning =>
        currentlyDisableWarnings := true
      | _ => ()
      };
      super.signature_item(self, item);
    };
    let signature = (self, signature: Typedtree.signature) => {
      let oldDisableWarnings = currentlyDisableWarnings^;
      super.signature(self, signature) |> ignore;
      currentlyDisableWarnings := oldDisableWarnings;
      signature;
    };

    {
      ...super,
      signature,
      signature_item,
      structure,
      structure_item,
      type_kind,
      value_binding,
      value_description,
    };
  };

  let structure = (~doGenType, structure) => {
    let collectExportLocations = collectExportLocations(~doGenType);
    structure
    |> collectExportLocations.structure(collectExportLocations)
    |> ignore;
  };
  let signature = signature => {
    let collectExportLocations = collectExportLocations(~doGenType=true);
    signature
    |> collectExportLocations.signature(collectExportLocations)
    |> ignore;
  };
};

/********   PROCESSING  ********/

let pathToString = path =>
  path |> List.rev_map(Name.toString) |> String.concat(".");

let pathWithoutHead = path => {
  switch (path |> List.rev_map(Name.toString)) {
  | [_, ...tl] => tl |> String.concat(".")
  | [] => ""
  };
};

let annotateAtEnd = (~pos) => !posIsReason(pos);

let getPosAnnotation = decl =>
  annotateAtEnd(~pos=decl.pos) ? decl.posEnd : decl.posStart;

let addDeclaration_ =
    (~sideEffects=false, ~declKind, ~path, ~loc: Location.t, name: Name.t) => {
  let pos = loc.loc_start;
  let posStart = pos;
  let posEnd = loc.loc_end;

  /* a .cmi file can contain locations from other files.
       For instance:
           module M : Set.S with type elt = int
       will create value definitions whose location is in set.mli
     */
  if (!loc.loc_ghost
      && (currentSrc^ == pos.pos_fname || currentModule^ === "*include*")) {
    if (debug^) {
      Log_.item(
        "add%sDeclaration %s %s path:%s@.",
        declKind == Value ? "Value" : "Type",
        name |> Name.toString,
        pos |> posToString,
        path |> pathToString,
      );
    };

    switch (path) {
    | [moduleName] when declKind == Value =>
      let oldSet = getDeclPositions(~moduleName);
      Hashtbl.replace(moduleDecls, moduleName, PosSet.add(pos, oldSet));
    | _ => ()
    };
    let decl = {
      declKind,
      path: [name, ...path],
      pos,
      posEnd,
      posStart,
      resolved: false,
      sideEffects,
    };
    PosHash.replace(decls, pos, decl);
  };
};

let addTypeDeclaration = addDeclaration_;

let addValueDeclaration = (~sideEffects, ~path, ~loc: Location.t, name) =>
  name |> addDeclaration_(~sideEffects, ~declKind=Value, ~path, ~loc);

/**** REPORTING ****/

let emitWarning = (~decl, ~message, ~name) => {
  Log_.info(~loc=decl |> declGetLoc, ~name, (ppf, ()) =>
    Format.fprintf(
      ppf,
      "@{<info>%s@} %s",
      decl.path |> pathWithoutHead,
      message,
    )
  );
};

module WriteDeadAnnotations = {
  type line = {
    mutable declarations: list(decl),
    original: string,
  };

  let rec lineToString_ = ({original, declarations}) => {
    switch (declarations) {
    | [] => original
    | [{declKind, path, pos} as decl, ...nextDeclarations] =>
      let isReason = posIsReason(pos);
      let annotationStr =
        (isReason ? "" : " ")
        ++ "["
        ++ (isReason || declKind != Value ? "@" : "@@")
        ++ deadAnnotation
        ++ " \""
        ++ (path |> pathWithoutHead)
        ++ "\"] ";
      let posAnnotation = decl |> getPosAnnotation;
      let col = posAnnotation.pos_cnum - posAnnotation.pos_bol;
      let originalLen = String.length(original);
      {
        original:
          if (String.length(original) >= col && col > 0) {
            let original1 = String.sub(original, 0, col);
            let original2 = String.sub(original, col, originalLen - col);
            original1 ++ annotationStr ++ original2;
          } else {
            isReason ? annotationStr ++ original : original ++ annotationStr;
          },
        declarations: nextDeclarations,
      }
      |> lineToString_;
    };
  };

  let lineToString = ({original, declarations}) => {
    let declarations =
      declarations
      |> List.sort((decl1, decl2) =>
           getPosAnnotation(decl2).pos_cnum
           - getPosAnnotation(decl1).pos_cnum
         );
    lineToString_({original, declarations});
  };

  let currentFile = ref("");
  let currentFileLines: ref(array(line)) = ref([||]);

  let readFile = fileName => {
    let channel = open_in(fileName);
    let lines = ref([]);
    [@raises End_of_file]
    let rec loop = () => {
      let line = {original: input_line(channel), declarations: []};
      lines := [line, ...lines^];
      loop();
    };
    try(loop()) {
    | End_of_file =>
      close_in_noerr(channel);
      lines^ |> List.rev |> Array.of_list;
    };
  };

  let writeFile = (fileName, lines) =>
    if (fileName != "" && write^) {
      let channel = open_out(fileName);
      let lastLine = Array.length(lines);
      lines
      |> Array.iteri((n, line) => {
           output_string(channel, line |> lineToString);
           if (n < lastLine - 1) {
             output_char(channel, '\n');
           };
         });
      close_out_noerr(channel);
    };

  let onDeadDecl = (~ppf, decl) => {
    let fileName = decl.pos.pos_fname;
    if (Sys.file_exists(fileName)) {
      if (fileName != currentFile^) {
        writeFile(currentFile^, currentFileLines^);
        currentFile := fileName;
        currentFileLines := readFile(fileName);
      };

      let indexInLines = (decl |> getPosAnnotation).pos_lnum - 1;

      switch (currentFileLines^[indexInLines]) {
      | line =>
        line.declarations = [decl, ...line.declarations];
        Format.fprintf(
          ppf,
          "  <-- line %d@.  %s@.",
          decl.pos.pos_lnum,
          line |> lineToString,
        );
      | exception (Invalid_argument(_)) =>
        Format.fprintf(ppf, "  <-- Can't find line %d@.", decl.pos.pos_lnum)
      };
    } else {
      Format.fprintf(ppf, "  <-- can't find file@.");
    };
  };

  let write = () => writeFile(currentFile^, currentFileLines^);
};

let declIsDead = (~refs, decl) => {
  let liveRefs =
    refs |> PosSet.filter(p => !ProcessDeadAnnotations.isAnnotatedDead(p));
  liveRefs
  |> PosSet.cardinal == 0
  && !ProcessDeadAnnotations.isAnnotatedGenTypeOrLive(decl.pos);
};

let doReportDead = pos =>
  !ProcessDeadAnnotations.isAnnotatedGenTypeOrDead(pos)
  && posInWhitelist(pos)
  && !posInBlacklist(pos);

let checkSideEffects = decl =>
  removeDeadValuesWithSideEffects || !decl.sideEffects;

let rec resolveRecursiveRefs =
        (
          ~deadDeclarations,
          ~level,
          ~orderedFiles,
          ~refs,
          ~refsBeingResolved,
          decl,
        )
        : bool => {
  switch (decl.pos) {
  | _ when decl.resolved =>
    if (recursiveDebug) {
      Log_.item(
        "recursiveDebug %s [%d] already resolved@.",
        decl.path |> pathToString,
        level,
      );
    };
    decl.pos |> ProcessDeadAnnotations.isAnnotatedDead;
  | _ when PosSet.mem(decl.pos, refsBeingResolved^) =>
    if (recursiveDebug) {
      Log_.item(
        "recursiveDebug %s [%d] is being resolved: assume dead@.",
        decl.path |> pathToString,
        level,
      );
    };
    true;
  | _ =>
    if (recursiveDebug) {
      Log_.item(
        "recursiveDebug resolving %s [%d]@.",
        decl.path |> pathToString,
        level,
      );
    };
    refsBeingResolved := PosSet.add(decl.pos, refsBeingResolved^);
    let allDepsResolved = ref(true);
    let newRefs =
      refs
      |> PosSet.filter(x =>
           if (x == decl.pos) {
             if (recursiveDebug) {
               Log_.item(
                 "recursiveDebug %s ignoring reference to self@.",
                 decl.path |> pathToString,
               );
             };
             false;
           } else {
             switch (PosHash.find_opt(decls, x)) {
             | None =>
               if (recursiveDebug) {
                 Log_.item(
                   "recursiveDebug can't find decl for %s@.",
                   x |> posToString,
                 );
               };
               true;
             | Some(xDecl) =>
               let xRefs =
                 PosHash.findSet(
                   xDecl.declKind == Value ? valueReferences : typeReferences,
                   x,
                 );
               let xDeclIsDead =
                 xDecl
                 |> resolveRecursiveRefs(
                      ~deadDeclarations,
                      ~level=level + 1,
                      ~orderedFiles,
                      ~refs=xRefs,
                      ~refsBeingResolved,
                    );
               if (!xDecl.resolved) {
                 allDepsResolved := false;
               };
               !xDeclIsDead;
             };
           }
         );

    let isDead = decl |> declIsDead(~refs=newRefs);
    let isResolved = !isDead || allDepsResolved^ || level == 0;

    if (isResolved) {
      decl.resolved = true;

      if (isDead) {
        if (decl.pos |> doReportDead) {
          deadDeclarations := [decl, ...deadDeclarations^];
        };
        if (decl |> checkSideEffects) {
          decl.pos |> ProcessDeadAnnotations.annotateDead;
        };
      } else if (decl.pos |> ProcessDeadAnnotations.isAnnotatedDead) {
        emitWarning(
          ~decl,
          ~message=" is annotated @dead but is live",
          ~name="Warning Incorrect Annotation",
        );
      };

      if (debug^) {
        let refsString =
          newRefs
          |> PosSet.elements
          |> List.map(posToString)
          |> String.concat(", ");
        Log_.item(
          "%s %s %s: %d references (%s) [%d]@.",
          isDead ? "Dead" : "Live",
          decl.declKind == Value ? "Value" : "Type",
          decl.path |> pathToString,
          newRefs |> PosSet.cardinal,
          refsString,
          level,
        );
      };
    };

    isDead;
  };
};

module Decl = {
  let compareUsingDependencies =
      (
        ~orderedFiles,
        {
          declKind: kind1,
          path: _path1,
          pos: {
            pos_fname: fname1,
            pos_lnum: lnum1,
            pos_bol: bol1,
            pos_cnum: cnum1,
          },
        },
        {
          declKind: kind2,
          path: _path2,
          pos: {
            pos_fname: fname2,
            pos_lnum: lnum2,
            pos_bol: bol2,
            pos_cnum: cnum2,
          },
        },
      ) => {
    [@raises Not_found]
    let findPosition = fn => Hashtbl.find(orderedFiles, fn);

    /* From the root of the file dependency DAG to the leaves.
       From the bottom of the file to the top. */
    let (position1, position2) =
      try((fname1 |> findPosition, fname2 |> findPosition)) {
      | Not_found => (0, 0)
      };
    compare(
      (position1, lnum2, bol2, cnum2, kind1),
      (position2, lnum1, bol1, cnum1, kind2),
    );
  };

  let compareForReporting =
      (
        {
          declKind: kind1,
          pos: {
            pos_fname: fname1,
            pos_lnum: lnum1,
            pos_bol: bol1,
            pos_cnum: cnum1,
          },
        },
        {
          declKind: kind2,
          pos: {
            pos_fname: fname2,
            pos_lnum: lnum2,
            pos_bol: bol2,
            pos_cnum: cnum2,
          },
        },
      ) => {
    compare(
      (fname1, lnum1, bol1, cnum1, kind1),
      (fname2, lnum2, bol2, cnum2, kind2),
    );
  };

  let isInsideReportedValue = decl => {
    let fileHasChanged = maxValuePosEnd^.pos_fname != decl.pos.pos_fname;

    let insideReportedValue =
      decl.declKind == Value
      && !fileHasChanged
      && maxValuePosEnd^.pos_cnum > decl.pos.pos_cnum;

    if (!insideReportedValue) {
      if (decl.declKind == Value) {
        if (fileHasChanged || decl.posEnd.pos_cnum > maxValuePosEnd^.pos_cnum) {
          maxValuePosEnd := decl.posEnd;
        };
      };
    };

    insideReportedValue;
  };

  let report = (~ppf, decl) => {
    let noSideEffectsOrUnderscore =
      !decl.sideEffects
      || (
        switch (decl.path) {
        | [hd, ..._] => hd |> Name.startsWithUnderscore
        | [] => false
        }
      );

    let (name, message) =
      switch (decl.declKind) {
      | Value => (
          "Warning Dead Value"
          ++ (!noSideEffectsOrUnderscore ? " With Side Effects" : ""),
          switch (decl.path) {
          | [name, ..._] when name |> Name.isUnderscore => "has no side effects and can be removed"
          | _ =>
            "is never used"
            ++ (
              !noSideEffectsOrUnderscore ? " and could have side effects" : ""
            )
          },
        )
      | RecordLabel => (
          "Warning Dead Type",
          "is a record label never used to read a value",
        )
      | VariantCase => (
          "Warning Dead Type",
          "is a variant case which is never constructed",
        )
      };

    let insideReportedValue = decl |> isInsideReportedValue;

    let shouldEmitWarning =
      !insideReportedValue
      && (
        switch (decl.path) {
        | [name, ..._] when name |> Name.isUnderscore => reportUnderscore
        | _ => true
        }
      );
    let shouldWriteAnnotation = shouldEmitWarning && decl |> checkSideEffects;
    if (shouldEmitWarning) {
      emitWarning(~decl, ~message, ~name);
    };
    if (shouldWriteAnnotation) {
      decl |> WriteDeadAnnotations.onDeadDecl(~ppf);
    };
  };
};

let reportDead = ppf => {
  let iterDeclInOrder = (~orderedFiles, ~deadDeclarations, decl) => {
    let refs =
      decl.pos
      |> PosHash.findSet(
           decl.declKind == Value ? valueReferences : typeReferences,
         );
    resolveRecursiveRefs(
      ~deadDeclarations,
      ~level=0,
      ~orderedFiles,
      ~refsBeingResolved=ref(PosSet.empty),
      ~refs,
      decl,
    )
    |> ignore;
  };

  if (debug^) {
    Log_.item("@.File References@.@.");
    fileReferences
    |> FileHash.iter((file, files) =>
         Log_.item(
           "%s -->> %s@.",
           file |> Filename.basename,
           files
           |> FileSet.elements
           |> List.map(Filename.basename)
           |> String.concat(", "),
         )
       );
  };

  let declarations =
    PosHash.fold(
      (_pos, decl, declarations) => [decl, ...declarations],
      decls,
      [],
    );

  let orderedFiles = Hashtbl.create(256);
  iterFilesFromRootsToLeaves(
    {
      let current = ref(0);
      fileName => {
        incr(current);
        Hashtbl.add(orderedFiles, fileName, current^);
      };
    },
  );

  let orderedDeclarations =
    declarations
    |> List.fast_sort(Decl.compareUsingDependencies(~orderedFiles)) /* analyze in reverse order */;

  let deadDeclarations = ref([]);
  orderedDeclarations
  |> List.iter(iterDeclInOrder(~orderedFiles, ~deadDeclarations));

  let sortedDeadDeclarations =
    deadDeclarations^ |> List.fast_sort(Decl.compareForReporting);
  sortedDeadDeclarations |> List.iter(Decl.report(~ppf));
};